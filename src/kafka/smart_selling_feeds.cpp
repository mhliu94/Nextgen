#include "kafka/smart_selling_feeds.h"

#include <chrono>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "algo/smart_selling/order_status.h"
#include "algo/smart_selling/runtime.h"

namespace ng::kafka {
namespace {

constexpr const char* kLog = "trading-ui.smart-selling-consumer";

std::string uuid_hex() { return uuid4_hex(); }

}  // namespace

SmartSellingFeed::SmartSellingFeed(std::string name, std::string topic, std::string bootstrap_servers,
                                   smart::SmartSellingRuntime& runtime, json client_config)
    : name_(std::move(name)),
      topic_(std::move(topic)),
      bootstrap_servers_(std::move(bootstrap_servers)),
      runtime_(runtime),
      client_config_(std::move(client_config)),
      group_id_("ktrader-smart-selling-" + name_ + "-" + uuid_hex()) {}

SmartSellingFeed::~SmartSellingFeed() { stop(); }

std::string SmartSellingFeed::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void SmartSellingFeed::set_state(const std::string& state) {
    {
        std::lock_guard lock(mutex_);
        if (state == state_) return;
        state_ = state;
    }
    runtime_.on_feed_state(name_, state);
}

void SmartSellingFeed::warn(const std::string& reason) {
    double now = mono_now();
    std::lock_guard lock(mutex_);
    if (now - last_warning_ >= 5) {
        last_warning_ = now;
        NG_LOG_WARN(kLog, "Smart selling {} feed: {}", name_, reason);
    }
}

void SmartSellingFeed::connected() {
    std::lock_guard lock(mutex_);
    if (assigned_.empty()) return;
    bool live = true;
    for (auto& tp : assigned_) if (!caught_up_.count(tp)) live = false;
    std::string next = live ? "live" : "replaying";
    if (next != state_) {
        state_ = next;
        // Notify outside the lock to avoid deadlocks with the runtime.
        mutex_.unlock();
        runtime_.on_feed_state(name_, next);
        mutex_.lock();
    }
}

void SmartSellingFeed::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    set_state("starting");
    thread_ = std::thread([this] { run(); });
}

void SmartSellingFeed::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    set_state("stopped");
}

void SmartSellingFeed::run() {
    while (!stop_.load()) {
        reconnect_requested_.store(false);
        std::unique_ptr<Consumer> consumer;
        try {
            Config conf;
            conf.apply(client_config_)
                .set("bootstrap.servers", bootstrap_servers_)
                .set("group.id", group_id_)
                .set("client.id", "ktrader-smart-selling-" + name_)
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
            Consumer::Options options;
            options.seek_to_beginning_on_assign = true;
            options.on_assign = [this](std::vector<TopicPartition>& partitions) {
                {
                    std::lock_guard lock(mutex_);
                    assigned_.clear();
                    caught_up_.clear();
                    for (auto& tp : partitions) assigned_.insert({tp.topic, tp.partition});
                }
                set_state("replaying");
            };
            options.on_revoke = [this](const std::vector<TopicPartition>&) {
                {
                    std::lock_guard lock(mutex_);
                    assigned_.clear();
                    caught_up_.clear();
                }
                set_state("starting");
            };
            options.on_error = [this](int code, const std::string& reason) {
                {
                    std::lock_guard lock(mutex_);
                    caught_up_.clear();
                }
                reconnect_requested_.store(true);
                set_state("disconnected");
                warn("Kafka connection error (" + std::to_string(code) + "): " + reason);
            };
            options.on_stats = [this](const std::string& stats_json) {
                json stats = json::parse(stats_json, nullptr, false);
                if (!stats.is_object()) {
                    warn("invalid Kafka connection statistics");
                    return;
                }
                const json* brokers = json_find(stats, "brokers");
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
                if (count > 0 && all_up) {
                    connected();
                } else if (count > 0) {
                    {
                        std::lock_guard lock(mutex_);
                        caught_up_.clear();
                    }
                    reconnect_requested_.store(true);
                    set_state("disconnected");
                }
            };
            consumer = std::make_unique<Consumer>(conf, options);
            consumer->subscribe({topic_});
            while (!stop_.load() && !reconnect_requested_.load()) {
                auto message = consumer->poll(1000);
                if (message && !reconnect_requested_.load()) handle_message(*message);
            }
        } catch (const std::exception& e) {
            {
                std::lock_guard lock(mutex_);
                caught_up_.clear();
            }
            set_state("disconnected");
            warn(std::string("consumer unavailable; reconnecting: ") + e.what());
        }
        if (consumer) {
            try {
                consumer->close();
            } catch (...) {
                warn("consumer close failed");
            }
            consumer.reset();
        }
        {
            std::lock_guard lock(mutex_);
            assigned_.clear();
            caught_up_.clear();
        }
        if (!stop_.load()) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }
    set_state("stopped");
}

void SmartSellingFeed::handle_message(const Message& message) {
    if (message.error) {
        {
            std::lock_guard lock(mutex_);
            caught_up_.clear();
        }
        reconnect_requested_.store(true);
        set_state("disconnected");
        warn("Kafka connection error (" + std::to_string(message.error_code) + "): " + message.error_text);
        return;
    }
    if (message.eof) {
        {
            std::lock_guard lock(mutex_);
            caught_up_.insert({message.topic, message.partition});
        }
        connected();
        return;
    }
    connected();
    messages_.fetch_add(1);
    try {
        deliver_message(message);
    } catch (const std::exception&) {
        rejected_.fetch_add(1);
        warn("invalid snapshot ignored");
    }
}

void SmartSellingFeed::deliver_message(const Message& message) {
    if (message.value.size() > smart::kMaxOrderMessageBytes) {
        throw std::invalid_argument("Snapshot must be at most 64 KiB of UTF-8 JSON");
    }
    if (name_ == "orders") {
        runtime_.on_order_payload(message.value);
        return;
    }
    json payload = json::parse(message.value);  // nlohmann rejects NaN/Infinity literals
    if (!payload.is_object()) throw std::invalid_argument("Volume snapshot must be an object");
    runtime_.on_volume_payload(payload, message.topic, message.partition, message.offset);
}

SmartSellingConsumer::SmartSellingConsumer(const AppConfig& config, const SmartSellingConfig& settings,
                                           smart::SmartSellingRuntime& runtime) {
    std::string bootstrap = config.kafka.bootstrap_servers;
    std::string market_bootstrap = config.kafka.market_data_bootstrap_servers;
    std::string order_topic = settings.order_status_topic;
    json order_ui_kafka = json::object();
    if (const json* k = json_find(config.order_ui, "kafka"); k && k->is_object()) order_ui_kafka = *k;
    if (order_topic.empty()) order_topic = json_to_string(json_find(order_ui_kafka, "topic")).value_or("order-status");
    std::string order_bootstrap = settings.order_status_bootstrap_servers.value_or(bootstrap);
    std::string volume_bootstrap = settings.trading_volume_bootstrap_servers.value_or(market_bootstrap);
    json order_client_config = settings.order_status_client_config;
    if (order_client_config.empty()) {
        if (const json* cc = json_find(order_ui_kafka, "client_config"); cc && cc->is_object()) order_client_config = *cc;
    }
    orders_ = std::make_unique<SmartSellingFeed>("orders", order_topic, order_bootstrap, runtime, order_client_config);
    volume_ = std::make_unique<SmartSellingFeed>("volume", settings.trading_volume_topic, volume_bootstrap, runtime,
                                                 settings.trading_volume_client_config);
}

void SmartSellingConsumer::start() {
    try {
        orders_->start();
        volume_->start();
    } catch (...) {
        orders_->stop();
        volume_->stop();
        throw;
    }
}

void SmartSellingConsumer::stop() {
    orders_->stop();
    volume_->stop();
}

}  // namespace ng::kafka
