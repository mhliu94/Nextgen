// Kafka consumers feeding the dashboard stores: account-details snapshots and the market-data
// cluster's price-books + stock-quotes topics (consumer_account_details.py / consumer_market_data.py).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/json.h"
#include "kafka/kafka_client.h"
#include "model/account.h"
#include "model/account_store.h"
#include "model/market_data.h"

namespace ng::kafka {

// Observable health of a consumer, shown in the UI status bar.
struct FeedHealth {
    std::string state = "starting";  // starting | live | disconnected | stopped
    std::optional<double> last_message_wall;
    std::uint64_t messages = 0;
    std::uint64_t rejected = 0;
    std::string last_error;
    json to_json() const;
};

class FeedHealthTracker {
public:
    void set_state(const std::string& state);
    void message(bool accepted);
    void error(const std::string& text);
    FeedHealth snapshot() const;

private:
    mutable std::mutex mutex_;
    FeedHealth health_;
};

class AccountDetailsConsumer {
public:
    AccountDetailsConsumer(const KafkaConfig& kafka, AccountStore& store, const AccountDirectory& accounts);
    ~AccountDetailsConsumer();
    void start();
    void stop();
    FeedHealth health() const { return health_.snapshot(); }
    // Exposed for tests: validate and store one decoded payload.
    bool handle_payload(const json& payload, const std::string& topic, int partition, long long offset);

private:
    void run();
    bool validate_snapshot(AccountSnapshot& snapshot, const std::string& topic, int partition, long long offset) const;

    KafkaConfig kafka_;
    AccountStore& store_;
    const AccountDirectory& accounts_;
    std::unique_ptr<Consumer> consumer_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    FeedHealthTracker health_;
};

class PriceBookConsumer {
public:
    using PayloadCallback = std::function<void(const json& payload, const std::string& topic, int partition, long long offset)>;
    using HealthCallback = std::function<void(bool healthy)>;
    using QuoteCallback = std::function<void(const StockQuote& quote)>;

    // `quote_callback` receives every parsed stock quote (market-activity history). With
    // `quote_history_seconds` > 0 the stock-quotes partitions are replayed from that long ago on each
    // assignment (by Kafka timestamp); price books replay at most kafka.market_data_replay_records.
    PriceBookConsumer(const KafkaConfig& kafka, MarketDataStore& store, PayloadCallback payload_callback = nullptr,
                      HealthCallback health_callback = nullptr, QuoteCallback quote_callback = nullptr,
                      double quote_history_seconds = 0);
    ~PriceBookConsumer();
    void start();
    void stop();
    FeedHealth health() const { return health_.snapshot(); }
    void handle_payload(const json& payload, const std::string& topic, int partition, long long offset);
    // Start offset for one assigned partition (exposed for tests): `watermarks` and `offset_for_time`
    // stand in for the broker lookups.
    using WatermarkLookup = std::function<std::optional<std::pair<long long, long long>>(const std::string&, int)>;
    using TimeLookup = std::function<std::optional<long long>(const std::string&, int, long long timestamp_ms)>;
    long long start_offset(const std::string& topic, int partition, double now_wall, const WatermarkLookup& watermarks,
                           const TimeLookup& offset_for_time) const;

private:
    void run();

    KafkaConfig kafka_;
    MarketDataStore& store_;
    PayloadCallback payload_callback_;
    HealthCallback health_callback_;
    QuoteCallback quote_callback_;
    double quote_history_seconds_ = 0;
    std::unique_ptr<Consumer> consumer_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    FeedHealthTracker health_;
};

}  // namespace ng::kafka
