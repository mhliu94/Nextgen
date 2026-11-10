#include "kafka/feeds.h"

#include <chrono>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"

namespace ng::kafka {
namespace {
constexpr const char* kAccountLog = "trading-ui.kafka.account-details";
constexpr const char* kMarketLog = "trading-ui.kafka.market-data";
}  // namespace

json FeedHealth::to_json() const {
    json out;
    out["state"] = state;
    out["last_message_at"] = last_message_wall ? json(iso_utc(*last_message_wall)) : json(nullptr);
    out["last_message_age_seconds"] = last_message_wall ? json(std::max(0.0, wall_now() - *last_message_wall)) : json(nullptr);
    out["messages"] = messages;
    out["rejected"] = rejected;
    out["last_error"] = last_error;
    return out;
}

void FeedHealthTracker::set_state(const std::string& state) {
    std::lock_guard lock(mutex_);
    health_.state = state;
}

void FeedHealthTracker::message(bool accepted) {
    std::lock_guard lock(mutex_);
    health_.messages += 1;
    if (!accepted) health_.rejected += 1;
    health_.last_message_wall = wall_now();
    health_.state = "live";
}

void FeedHealthTracker::error(const std::string& text) {
    std::lock_guard lock(mutex_);
    health_.last_error = text;
    health_.state = "disconnected";
}

FeedHealth FeedHealthTracker::snapshot() const {
    std::lock_guard lock(mutex_);
    return health_;
}

// ---------------------------------------------------------------------------
// AccountDetailsConsumer
// ---------------------------------------------------------------------------

AccountDetailsConsumer::AccountDetailsConsumer(const KafkaConfig& kafka, AccountStore& store, const AccountDirectory& accounts)
    : kafka_(kafka), store_(store), accounts_(accounts) {}

AccountDetailsConsumer::~AccountDetailsConsumer() { stop(); }

void AccountDetailsConsumer::start() {
    Config conf;
    conf.set("bootstrap.servers", kafka_.bootstrap_servers)
        .set("group.id", kafka_.group_id)
        .set("enable.auto.commit", "true")
        .set("auto.offset.reset", kafka_.auto_offset_reset)
        .set("session.timeout.ms", "10000")
        .set("max.poll.interval.ms", "300000")
        .set("client.id", "ktrader-nextgen-account-details")
        .set("log_level", "4")
        .apply(kafka_.client_config)
        .route_logs(kAccountLog);
    Consumer::Options options;
    options.on_error = [this](int code, const std::string& reason) {
        health_.error(reason);
        NG_LOG_WARN(kAccountLog, "account-details consumer error ({}): {}", code, reason);
    };
    consumer_ = std::make_unique<Consumer>(conf, options);
    consumer_->subscribe({kafka_.account_details_topic});
    NG_LOG_INFO(kAccountLog, "account-details consuming topic={} bootstrap={} group={}", kafka_.account_details_topic,
                kafka_.bootstrap_servers, kafka_.group_id);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void AccountDetailsConsumer::stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (consumer_) {
        consumer_->close();
        consumer_.reset();
    }
    health_.set_state("stopped");
}

void AccountDetailsConsumer::run() {
    int timeout_ms = static_cast<int>(std::max(0.05, kafka_.poll_timeout_sec) * 1000);
    while (!stop_.load()) {
        std::optional<Message> msg;
        try {
            msg = consumer_->poll(timeout_ms);
        } catch (const std::exception& e) {
            health_.error(e.what());
            NG_LOG_ERROR(kAccountLog, "poll failed: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (!msg) continue;
        if (msg->error) {
            health_.error(msg->error_text);
            NG_LOG_WARN(kAccountLog, "account-details consume error: {}", msg->error_text);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (msg->eof || msg->value.empty()) continue;
        json payload = json::parse(msg->value, nullptr, false);
        if (payload.is_discarded()) {
            health_.message(false);
            NG_LOG_WARN(kAccountLog, "Ignoring malformed account-details Kafka message topic={} partition={} offset={}", msg->topic,
                        msg->partition, msg->offset);
            continue;
        }
        bool accepted = handle_payload(payload, msg->topic, msg->partition, msg->offset);
        health_.message(accepted);
    }
}

bool AccountDetailsConsumer::handle_payload(const json& payload, const std::string& topic, int partition, long long offset) {
    auto snapshot = parse_snapshot_obj(payload);
    if (!snapshot) {
        NG_LOG_WARN(kAccountLog, "Ignoring malformed account-details Kafka message topic={} partition={} offset={}", topic, partition,
                    offset);
        return false;
    }
    if (!validate_snapshot(*snapshot, topic, partition, offset)) return false;
    store_.upsert(std::move(*snapshot));
    return true;
}

bool AccountDetailsConsumer::validate_snapshot(AccountSnapshot& snap, const std::string& topic, int partition,
                                               long long offset) const {
    std::string account_id = trim(snap.account_id);
    const AccountMeta* meta = accounts_.find(account_id);
    if (!meta) {
        NG_LOG_WARN(kAccountLog,
                    "Ignoring account snapshot with unknown account_id={} account_num_id={} topic={} partition={} offset={}",
                    account_id, snap.account_num_id ? std::to_string(*snap.account_num_id) : "None", topic, partition, offset);
        return false;
    }
    snap.account_id = account_id;
    if (!snap.account_num_id) {
        snap.account_num_id = meta->num_id;
        return true;
    }
    const AccountMeta* expected = accounts_.find_by_num(*snap.account_num_id);
    if (!expected) {
        NG_LOG_WARN(kAccountLog,
                    "Ignoring account snapshot with unknown account_num_id={} account_id={} topic={} partition={} offset={}",
                    *snap.account_num_id, account_id, topic, partition, offset);
        return false;
    }
    if (expected->id != account_id || *snap.account_num_id != meta->num_id) {
        NG_LOG_WARN(kAccountLog,
                    "Ignoring account snapshot with mismatched account identifiers account_id={} account_num_id={} "
                    "expected_account_id={} expected_num_id={} topic={} partition={} offset={}",
                    account_id, *snap.account_num_id, expected->id, meta->num_id, topic, partition, offset);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PriceBookConsumer
// ---------------------------------------------------------------------------

PriceBookConsumer::PriceBookConsumer(const KafkaConfig& kafka, MarketDataStore& store, PayloadCallback payload_callback,
                                     HealthCallback health_callback, QuoteCallback quote_callback, double quote_history_seconds)
    : kafka_(kafka),
      store_(store),
      payload_callback_(std::move(payload_callback)),
      health_callback_(std::move(health_callback)),
      quote_callback_(std::move(quote_callback)),
      quote_history_seconds_(quote_history_seconds) {}

long long PriceBookConsumer::start_offset(const std::string& topic, int partition, double now_wall,
                                          const WatermarkLookup& watermarks, const TimeLookup& offset_for_time) const {
    if (quote_history_seconds_ > 0 && topic == kafka_.stock_quotes_topic) {
        long long timestamp_ms = static_cast<long long>((now_wall - quote_history_seconds_) * 1000);
        if (auto offset = offset_for_time(topic, partition, timestamp_ms)) return *offset;
        NG_LOG_WARN(kMarketLog, "Quote history offsets unavailable; falling back to bounded recent replay");
    }
    // These records are complete snapshots. Bound the warm-up on every assignment, including
    // restarts and rebalances, regardless of commits.
    int replay_records = kafka_.market_data_replay_records;
    if (replay_records == 0) return kOffsetEnd;
    auto marks = watermarks(topic, partition);
    if (!marks) {
        NG_LOG_WARN(kMarketLog, "market-data offsets unavailable for {}[{}]; waiting for new data", topic, partition);
        return kOffsetEnd;
    }
    // The high watermark is the next offset after the latest record.
    return std::max(marks->first, marks->second - replay_records);
}

PriceBookConsumer::~PriceBookConsumer() { stop(); }

void PriceBookConsumer::start() {
    if (kafka_.market_data_topic == kafka_.stock_quotes_topic) {
        throw std::invalid_argument("stock_quotes_topic must differ from market_data_topic");
    }
    Config conf;
    conf.set("bootstrap.servers", kafka_.market_data_bootstrap_servers)
        .set("group.id", kafka_.market_data_group_id)
        .set("enable.auto.commit", "false")
        .set("auto.offset.reset", kafka_.market_data_auto_offset_reset)
        .set("session.timeout.ms", "10000")
        .set("max.poll.interval.ms", "300000")
        .set("client.id", "ktrader-nextgen-market-data")
        .set("fetch.wait.max.ms", "50")
        .set("log_level", "4")
        .apply(kafka_.market_data_client_config)
        .route_logs(kMarketLog);
    Consumer::Options options;
    options.on_assign = [this](std::vector<TopicPartition>& partitions) {
        double now = wall_now();
        std::string assigned;
        for (auto& tp : partitions) {
            tp.offset = start_offset(
                tp.topic, tp.partition, now,
                [this](const std::string& topic, int partition) { return consumer_->query_watermark_offsets(topic, partition, 5000); },
                [this](const std::string& topic, int partition, long long timestamp_ms) {
                    return consumer_->offset_for_time(topic, partition, timestamp_ms, 5000);
                });
            if (!assigned.empty()) assigned += ", ";
            assigned += tp.topic + "[" + std::to_string(tp.partition) + "]@" + std::to_string(tp.offset);
        }
        NG_LOG_INFO(kMarketLog, "market-data assigned: {}", assigned);
    };
    options.on_error = [this](int code, const std::string& reason) {
        health_.error(reason);
        if (health_callback_) health_callback_(false);
        NG_LOG_WARN(kMarketLog, "market-data consumer error ({}): {}", code, reason);
    };
    consumer_ = std::make_unique<Consumer>(conf, options);
    consumer_->subscribe({kafka_.market_data_topic, kafka_.stock_quotes_topic});
    NG_LOG_INFO(kMarketLog, "market-data consuming topics=[{}, {}] bootstrap={}", kafka_.market_data_topic, kafka_.stock_quotes_topic,
                kafka_.market_data_bootstrap_servers);
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void PriceBookConsumer::stop() {
    stop_.store(true);
    if (health_callback_ && thread_.joinable()) health_callback_(false);
    if (thread_.joinable()) thread_.join();
    if (consumer_) {
        consumer_->close();
        consumer_.reset();
    }
    health_.set_state("stopped");
}

void PriceBookConsumer::run() {
    int timeout_ms = static_cast<int>(std::max(0.05, kafka_.poll_timeout_sec) * 1000);
    while (!stop_.load()) {
        std::optional<Message> msg;
        try {
            msg = consumer_->poll(timeout_ms);
        } catch (const std::exception& e) {
            health_.error(e.what());
            if (health_callback_) health_callback_(false);
            NG_LOG_ERROR(kMarketLog, "poll failed: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (!msg) continue;
        if (msg->error) {
            health_.error(msg->error_text);
            if (health_callback_) health_callback_(false);
            NG_LOG_WARN(kMarketLog, "market-data consume error: {}", msg->error_text);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (msg->eof || msg->value.empty()) continue;
        json payload = json::parse(msg->value, nullptr, false);
        if (!payload.is_object()) {
            health_.message(false);
            continue;
        }
        try {
            handle_payload(payload, msg->topic, msg->partition, msg->offset);
            health_.message(true);
        } catch (const std::exception& e) {
            health_.message(false);
            NG_LOG_WARN(kMarketLog, "Invalid market-data record on topic={}: {}", msg->topic, e.what());
        }
    }
}

void PriceBookConsumer::handle_payload(const json& payload, const std::string& topic, int partition, long long offset) {
    if (topic == kafka_.stock_quotes_topic) {
        auto quote = stock_quote_from_payload(payload);
        if (quote) {
            store_.upsert_stock_quote(*quote);
            if (quote_callback_) {
                try {
                    quote_callback_(*quote);
                } catch (const std::exception& e) {
                    NG_LOG_ERROR(kMarketLog, "Could not persist market quote history: {}", e.what());
                }
            }
        }
        return;
    }
    if (topic != kafka_.market_data_topic) return;
    auto row = quote_row_from_price_book(payload);
    if (row) store_.upsert(*row);
    auto book = order_book_from_price_book(payload, 20);
    if (book) store_.upsert_book(std::move(*book));
    if (payload_callback_) {
        try {
            if (health_callback_) health_callback_(true);
            payload_callback_(payload, topic, partition, offset);
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kMarketLog, "Smart selling rejected a price-book update: {}", e.what());
            if (health_callback_) health_callback_(false);
        }
    }
}

}  // namespace ng::kafka
