#include "order_ui/consumer.h"

#include <chrono>

#include "core/logging.h"
#include "core/sqlite.h"
#include "core/strings.h"

namespace ng::order_ui {
namespace {
constexpr const char* kLog = "order-ui.consumer";
}

OrderStatusConsumer::OrderStatusConsumer(OrderKafkaConfig config, OrderStore& store, std::string client_id)
    : config_(std::move(config)),
      store_(store),
      client_id_(std::move(client_id)),
      // Independent dashboards must not divide the topic's partitions between them.
      group_id_(config_.group_id + "-" + uuid4_hex()) {}

OrderStatusConsumer::~OrderStatusConsumer() { stop(); }

std::unique_ptr<kafka::Consumer> OrderStatusConsumer::create_consumer() {
    kafka::Config conf;
    conf.apply(config_.client_config)
        .set("bootstrap.servers", config_.bootstrap_servers)
        .set("group.id", group_id_)
        .set("client.id", client_id_)
        .set("enable.auto.commit", "false")
        .set("enable.auto.offset.store", "false")
        .set("auto.offset.reset", "earliest")
        .set("enable.partition.eof", "true")
        .set("allow.auto.create.topics", "false")
        .set("partition.assignment.strategy", "range")
        .set("group.protocol", "classic")
        .set("session.timeout.ms", "10000")
        .set("statistics.interval.ms", "5000")
        .set("log_level", "4")
        .route_logs(kLog);
    kafka::Consumer::Options options;
    options.seek_to_beginning_on_assign = true;
    options.on_assign = [this](std::vector<kafka::TopicPartition>& partitions) {
        {
            std::lock_guard lock(mutex_);
            assigned_.clear();
            caught_up_.clear();
            for (auto& tp : partitions) assigned_.insert({tp.topic, tp.partition});
        }
        store_.update_feed(std::string("replaying"), std::string("Loading retained order snapshots"));
    };
    options.on_revoke = [this](const std::vector<kafka::TopicPartition>&) {
        {
            std::lock_guard lock(mutex_);
            assigned_.clear();
            caught_up_.clear();
        }
        store_.update_feed(std::string("starting"), std::string("Waiting for Kafka partitions"));
    };
    options.on_error = [this](int code, const std::string&) { on_error(code); };
    options.on_stats = [this](const std::string& raw) {
        json stats = json::parse(raw, nullptr, false);
        const json* brokers = stats.is_object() ? json_find(stats, "brokers") : nullptr;
        int count = 0;
        bool all_up = true;
        if (brokers && brokers->is_object()) {
            for (auto it = brokers->begin(); it != brokers->end(); ++it) {
                const json* nodeid = json_find(it.value(), "nodeid");
                if (!nodeid || !nodeid->is_number() || nodeid->get<double>() < 0) continue;
                ++count;
                const json* state = json_find(it.value(), "state");
                if (!state || !state->is_string() || state->get<std::string>() != "UP") all_up = false;
            }
        }
        if (count > 0 && all_up) connected();
        else if (count > 0) store_.update_feed(std::string("disconnected"), std::string("Kafka connection interrupted; reconnecting"));
    };
    return std::make_unique<kafka::Consumer>(conf, options);
}

void OrderStatusConsumer::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    consumer_ = create_consumer();
    thread_ = std::thread([this] { run(); });
}

void OrderStatusConsumer::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

void OrderStatusConsumer::on_error(int code) {
    // Client errors can contain connection details; the browser gets a generic message.
    NG_LOG_WARN(kLog, "Order Kafka connection error (code {})", code);
    store_.update_feed(std::string("disconnected"), std::string("Kafka unavailable; reconnecting"));
}

void OrderStatusConsumer::connected() {
    bool live;
    {
        std::lock_guard lock(mutex_);
        if (assigned_.empty()) return;
        live = true;
        for (auto& tp : assigned_) {
            if (!caught_up_.count(tp)) live = false;
        }
    }
    if (live) store_.update_feed(std::string("live"), std::string("Kafka connected"));
    else store_.update_feed(std::string("replaying"), std::string("Loading retained order snapshots"));
}

void OrderStatusConsumer::handle_value(const std::string& raw, const std::string& topic, int partition, long long offset) {
    try {
        store_.upsert(orders::parse_order_message(raw));
    } catch (const SqliteError&) {
        // Storage trouble is not a malformed snapshot: let the consumer loop reconnect and replay.
        throw;
    } catch (const std::exception& e) {
        store_.reject_message();
        // Do not log arbitrary producer data.
        NG_LOG_WARN(kLog, "Ignoring invalid order snapshot topic={} partition={} offset={} ({})", topic, partition, offset, e.what());
    }
}

void OrderStatusConsumer::handle_message(const kafka::Message& message) {
    if (message.eof) {
        {
            std::lock_guard lock(mutex_);
            caught_up_.insert({message.topic, message.partition});
        }
        connected();
        return;
    }
    if (message.error) {
        on_error(message.error_code);
        return;
    }
    connected();
    store_.received();
    if (message.value.empty()) {
        // Tombstones are not order snapshots.
        store_.reject_message();
        NG_LOG_WARN(kLog, "Ignoring invalid order snapshot topic={} partition={} offset={} (tombstone)", message.topic,
                    message.partition, message.offset);
        return;
    }
    handle_value(message.value, message.topic, message.partition, message.offset);
}

void OrderStatusConsumer::run() {
    int timeout_ms = static_cast<int>(config_.poll_timeout_sec * 1000);
    while (!stop_.load()) {
        try {
            if (!consumer_) consumer_ = create_consumer();
            consumer_->subscribe({config_.topic});
            while (!stop_.load()) {
                auto message = consumer_->poll(timeout_ms);
                if (message) handle_message(*message);
            }
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "Order consumer failed; reconnecting: {}", e.what());
            store_.update_feed(std::string("disconnected"), std::string("Kafka unavailable; reconnecting"));
        }
        if (consumer_) {
            try {
                consumer_->close();
            } catch (const std::exception& e) {
                NG_LOG_ERROR(kLog, "Could not close the order consumer: {}", e.what());
            }
            consumer_.reset();
        }
        {
            std::lock_guard lock(mutex_);
            assigned_.clear();
            caught_up_.clear();
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    store_.update_feed(std::string("stopped"), std::string("Order consumer stopped"));
}

}  // namespace ng::order_ui
