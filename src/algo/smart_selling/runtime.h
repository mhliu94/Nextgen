// Control-server wiring for Smart selling (smart_selling_runtime.py): one engine, one feedback
// correlator, one market adapter, a 1 Hz heartbeat and a single publisher worker.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "model/account.h"
#include "algo/smart_selling/engine.h"
#include "algo/smart_selling/feedback.h"
#include "algo/smart_selling/market_adapter.h"

namespace ng::smart {

class SmartSellingRuntime {
public:
    using MetasProvider = std::function<const AccountDirectory&()>;
    using SnapshotsProvider = std::function<std::map<std::string, AccountSnapshot>()>;
    using PublishCommand = std::function<void(const json& command, const std::string& key)>;
    using FastConflict = std::function<bool(const std::string& account_id, const std::string& symbol)>;
    using FastActive = std::function<bool()>;
    using Clock = std::function<double()>;

    SmartSellingRuntime(const SmartSellingConfig& config, MetasProvider metas, SnapshotsProvider snapshots,
                        PublishCommand publish, FastConflict fast_conflict = nullptr, FastActive fast_active = nullptr,
                        std::shared_ptr<SmartSellingEngine> engine = nullptr, Clock clock = nullptr, Clock wall_clock = nullptr);
    ~SmartSellingRuntime();

    void start();
    void close();

    json start_parent(const json& settings);
    bool has_active_session();
    bool owns_account(const std::string& account_id);
    bool owns(const std::string& account_id, const std::string& symbol);
    std::vector<json> list_parents();
    json stop_parent(const std::string& parent_id);
    int stop_all();
    void on_price_book(const json& payload, const std::optional<std::string>& topic = std::nullopt,
                       std::optional<int> partition = std::nullopt, std::optional<long long> offset = std::nullopt);
    void on_price_book_health(bool healthy);
    void on_volume_payload(const json& payload, const std::optional<std::string>& topic = std::nullopt,
                           std::optional<int> partition = std::nullopt, std::optional<long long> offset = std::nullopt);
    void on_order_payload(const std::string& raw);
    void track_external_order(const json& command);
    void on_feed_state(const std::string& feed, const std::string& state);
    std::map<std::string, std::string> feed_states();
    void set_feed_states_for_test(const std::map<std::string, std::string>& states);
    void tick();
    // Blocks until the publisher worker has drained every queued action (tests).
    void wait_idle();

    SmartSellingEngine& engine() { return *engine_; }
    SmartSellingFeedback& feedback() { return *feedback_; }
    SmartSellingMarketAdapter& market() { return *market_; }
    const SmartSellingConfig& config() const { return config_; }
    SmartSellingConfig& mutable_config() { return config_; }

private:
    Decimal available_position(const std::string& account_id, const std::string& symbol);
    static std::vector<std::string> parent_accounts(const json& parent);
    void dispatch(const std::vector<json>& actions);
    std::optional<json> prepare_submit(const json& action);
    void publish_action(const json& action);
    void handoff_action(json action);
    void heartbeat_loop();
    void worker_loop();

    SmartSellingConfig config_;
    MetasProvider metas_;
    SnapshotsProvider snapshots_;
    PublishCommand publish_;
    FastConflict fast_conflict_;
    FastActive fast_active_;
    Clock clock_;
    Clock wall_clock_;
    std::recursive_mutex mutex_;
    std::shared_ptr<SmartSellingEngine> engine_;
    std::unique_ptr<SmartSellingFeedback> feedback_;
    std::unique_ptr<SmartSellingMarketAdapter> market_;
    std::map<std::string, std::string> feeds_{{"orders", "starting"}, {"volume", "starting"}};
    int lock_fd_ = -1;
    // True when this runtime created the engine (an injected engine belongs to the caller).
    bool owns_engine_ = false;
    std::thread heartbeat_;
    std::thread worker_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<json> queue_;
    int in_flight_ = 0;
    std::atomic<bool> stop_{false};
    bool closed_ = false;
    double last_forecast_ = 0.0;
};

}  // namespace ng::smart
