// Application services and the control logic that used to live in trading_ui/webserver.py:
// ownership guards between manual orders and algorithm sessions, the single-session rule
// across E/F/G, and the JSON views served to the browser.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "algo/fast_trading/fast_trading.h"
#include "kafka/feeds.h"
#include "kafka/producer.h"
#include "model/account.h"
#include "model/account_store.h"
#include "model/market_data.h"
#include "ops/operations_log.h"
#include "algo/smart_selling/runtime.h"
#include "web/event_hub.h"

namespace ng::web {

// Where published commands go (Kafka in production, a capture list in tests).
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual void publish(const json& command, const std::string& key) = 0;
    virtual json stats() const { return json::object(); }
    virtual std::vector<kafka::PublishedCommand> history() const { return {}; }
};

class KafkaCommandSink : public CommandSink {
public:
    explicit KafkaCommandSink(const KafkaConfig& kafka) : producer_(kafka) {}
    void publish(const json& command, const std::string& key) override { producer_.publish_order(command, key); }
    json stats() const override { return producer_.stats(); }
    std::vector<kafka::PublishedCommand> history() const override { return producer_.history(); }
    kafka::TradingCommandsProducer& producer() { return producer_; }

private:
    kafka::TradingCommandsProducer producer_;
};

// Records every published command in memory (tests and the "recent commands" view).
class RecordingCommandSink : public CommandSink {
public:
    void publish(const json& command, const std::string& key) override;
    std::vector<kafka::PublishedCommand> history() const override;
    json stats() const override;
    void fail_next(const std::string& error) { fail_next_ = error; }
    void clear();
    std::size_t count() const;

private:
    mutable std::mutex mutex_;
    std::vector<kafka::PublishedCommand> history_;
    std::string fail_next_;
};

struct FeedHealthProviders {
    std::function<json()> accounts;
    std::function<json()> market;
    std::function<json()> orders;
    std::function<json()> volume;
};

struct StopAllResult {
    int fast_stopped = 0;
    int smart_stopped = 0;
    bool notification_published = false;
    json notification;
    std::string notification_error;
};

class ControlCenter {
public:
    ControlCenter(AppConfig config, AccountDirectory accounts, std::vector<std::string> symbols);
    ~ControlCenter();

    // Wiring (called once by the application before serving).
    void set_command_sink(std::unique_ptr<CommandSink> sink);
    void set_operations_log(std::unique_ptr<OperationsLog> log);
    // Status of the persistence services (order archive, holdings recorder) for /api/health.
    void set_persistence_status(std::function<json()> provider) { persistence_status_ = std::move(provider); }
    void create_fast_trading_manager();
    void create_smart_selling_runtime(std::shared_ptr<smart::SmartSellingEngine> engine = nullptr);
    void set_feed_health_providers(FeedHealthProviders providers);
    void start_background();  // starts the smart heartbeat and hub wiring
    void shutdown();

    // Accessors.
    const AppConfig& config() const { return config_; }
    const AccountDirectory& accounts() const { return accounts_; }
    const std::vector<std::string>& symbols() const { return symbols_; }
    AccountStore& account_store() { return account_store_; }
    MarketDataStore& market_store() { return market_store_; }
    HistoricalCloseStore& historical_closes() { return *historical_; }
    EventHub& hub() { return hub_; }
    CommandSink* command_sink() { return sink_.get(); }
    fast::StrategyManager* fast_manager() { return fast_.get(); }
    smart::SmartSellingRuntime* smart_runtime() { return smart_.get(); }
    OperationsLog* operations_log() { return operations_log_.get(); }
    double started_at() const { return started_at_; }

    // Snapshots restricted to configured accounts (get_served_snapshots).
    std::map<std::string, AccountSnapshot> served_snapshots() const;
    std::optional<double> market_last(const std::string& symbol) const;

    // Publishing paths (webserver._publish_*). Throw std::invalid_argument / std::runtime_error.
    void publish_control_command(const json& command, const std::string& key);
    void publish_fast_trading_command(const json& command, const std::string& key);
    void publish_trading_command(const json& command, const std::string& key);
    std::string start_fast_strategy(const json& command);
    json start_smart_parent(const json& settings);
    json stop_smart_parent(const std::string& parent_id);
    StopAllResult stop_all_algorithms(const std::optional<std::string>& reason);
    bool any_algorithm_active() const;

    // JSON views.
    json accounts_payload(const std::string& lang) const;   // /api/accounts
    json accounts_view(const std::string& lang) const;      // accounts + metas + portfolio + stock quotes
    json market_rows(const std::vector<std::string>& symbols) const;
    json market_payload(const std::optional<std::string>& symbol) const;  // /api/market-data
    json insights_payload(const std::string& symbol, int depth) const;    // /api/market-insights
    std::vector<std::string> insight_symbols() const;
    json smart_status() const;
    json algo_status() const;
    json feeds_health() const;
    json health() const;
    json commands_history() const;
    json bootstrap(const std::string& lang, const std::string& user) const;

    // Serialized JSON shared by every HTTP and stream consumer until the underlying data changes:
    // one serialization per data version (or per 250 ms for status documents) instead of one per
    // client per event.
    std::shared_ptr<const std::string> accounts_payload_text(const std::string& lang) const;
    std::shared_ptr<const std::string> accounts_view_text(const std::string& lang) const;
    std::shared_ptr<const std::string> market_payload_text() const;
    std::shared_ptr<const std::string> insights_payload_text(const std::string& symbol, int depth) const;
    std::shared_ptr<const std::string> smart_status_text() const;
    std::shared_ptr<const std::string> algo_status_text() const;
    std::shared_ptr<const std::string> feeds_health_text() const;
    std::shared_ptr<const std::string> commands_history_text() const;
    std::shared_ptr<const std::string> health_text() const;
    int max_insight_levels() const { return config_.market_data.market_insights_max_levels; }

private:
    void require_idle_algorithms() const;
    bool owned_by_algorithm(const std::string& account_id) const;

    AppConfig config_;
    AccountDirectory accounts_;
    std::vector<std::string> symbols_;
    AccountStore account_store_;
    MarketDataStore market_store_;
    std::unique_ptr<HistoricalCloseStore> historical_;
    std::unique_ptr<CommandSink> sink_;
    std::unique_ptr<OperationsLog> operations_log_;
    std::unique_ptr<fast::StrategyManager> fast_;
    std::unique_ptr<smart::SmartSellingRuntime> smart_;
    FeedHealthProviders feed_health_;
    EventHub hub_;
    std::function<json()> persistence_status_;
    // Bumped on every hub notification so status caches never serve a stale document after a change.
    std::atomic<std::uint64_t> status_generation_{0};
    mutable std::mutex text_cache_mutex_;
    mutable std::map<std::string, std::pair<std::uint64_t, std::shared_ptr<const std::string>>> text_cache_;
    void notify_hub(unsigned topics, const std::string& symbol = "");
    std::shared_ptr<const std::string> cached_text(const std::string& name, std::uint64_t key,
                                                   const std::function<json()>& build) const;
    mutable std::recursive_mutex algo_lock_;
    double started_at_;
    std::atomic<bool> background_started_{false};
    std::thread status_thread_;
    std::atomic<bool> stop_{false};
};

// Pure view helpers (testable without a ControlCenter).
json portfolio_view(const std::map<std::string, AccountSnapshot>& accounts, const AccountDirectory& metas,
                    const std::map<std::string, StockQuote>& quotes);
bool snapshot_is_stale(const std::optional<std::string>& ts, double now_epoch);
json account_meta_view(const AccountMeta& meta);

}  // namespace ng::web
