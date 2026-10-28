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

    PriceBookConsumer(const KafkaConfig& kafka, MarketDataStore& store, PayloadCallback payload_callback = nullptr,
                      HealthCallback health_callback = nullptr);
    ~PriceBookConsumer();
    void start();
    void stop();
    FeedHealth health() const { return health_.snapshot(); }
    void handle_payload(const json& payload, const std::string& topic, int partition, long long offset);

private:
    void run();

    KafkaConfig kafka_;
    MarketDataStore& store_;
    PayloadCallback payload_callback_;
    HealthCallback health_callback_;
    std::unique_ptr<Consumer> consumer_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    FeedHealthTracker health_;
};

}  // namespace ng::kafka
