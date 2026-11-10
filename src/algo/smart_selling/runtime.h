// Control-server wiring for the Smart controllers (smart_selling_runtime.py): one engine, one
// feedback correlator, one market adapter, a 1 Hz heartbeat and a single publisher worker.
// SmartSellingRuntime is mode G; SmartBuyingRuntime (algo/smart_buying/runtime.h) derives from it
// for mode H and replaces only the resource checks and the session sizing.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "model/account.h"
#include "algo/smart_selling/engine.h"
#include "algo/smart_selling/feedback.h"
#include "algo/smart_selling/market_adapter.h"
#include "reporting/session_reporter.h"

namespace ng::smart {

class SmartSellingRuntime {
public:
    using MetasProvider = std::function<const AccountDirectory&()>;
    using SnapshotsProvider = std::function<std::map<std::string, AccountSnapshot>()>;
    using PublishCommand = std::function<void(const json& command, const std::string& key)>;
    // True when another algorithm (fast trading E/F or the other Smart controller) owns the account.
    using OwnsAccount = std::function<bool(const std::string& account_id)>;
    using Clock = std::function<double()>;

    SmartSellingRuntime(const SmartSellingConfig& config, MetasProvider metas, SnapshotsProvider snapshots,
                        PublishCommand publish, OwnsAccount fast_owns_account = nullptr,
                        std::shared_ptr<SmartSellingEngine> engine = nullptr, Clock clock = nullptr, Clock wall_clock = nullptr,
                        reporting::SessionReporter* reporting = nullptr);
    virtual ~SmartSellingRuntime();

    void start();
    void close();

    // "G" (Smart selling) or "H" (Smart buying).
    const std::string& trading_mode() const { return trading_mode_; }

    virtual json start_parent(const json& settings);
    bool has_active_session();
    // Stopping and reconciling sessions retain ownership until all children resolve.
    bool owns_account(const std::string& account_id);
    bool owns(const std::string& account_id, const std::string& symbol);
    // Recent history plus every active session (older active sessions stay visible).
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

    // Position-weighted average cost of the account's long holdings of `symbol`; nullopt when any
    // matching holding has no usable cost (unknown is never treated as zero). Exposed for tests.
    std::optional<Decimal> account_average_cost(const std::string& account_id, const std::string& symbol);

protected:
    using EngineFactory =
        std::function<std::shared_ptr<SmartSellingEngine>(const std::string& path, const Clock& clock, const Clock& wall_clock)>;
    // Shared constructor: `engine_factory` builds the file-backed engine when none is injected.
    SmartSellingRuntime(const SmartSellingConfig& config, MetasProvider metas, SnapshotsProvider snapshots,
                        PublishCommand publish, OwnsAccount fast_owns_account, std::shared_ptr<SmartSellingEngine> engine,
                        Clock clock, Clock wall_clock, reporting::SessionReporter* reporting, std::string trading_mode,
                        EngineFactory engine_factory);

    // The shared heartbeat calls this resource check before each decision: sellable shares for
    // mode G, available USD cash for mode H. Throws ValueError when the snapshot is unusable.
    virtual Decimal available_position(const std::string& account_id, const std::string& symbol);
    // Last local checks before an unsent intent is handed to the publisher.
    virtual std::optional<json> prepare_submit(const json& action);

    static std::vector<std::string> parent_accounts(const json& parent);
    bool feeds_live() const;
    void dispatch(const std::vector<json>& actions);
    void publish_action(const json& action);
    void handoff_action(json action);
    void heartbeat_loop();
    void worker_loop();
    // Mirrors session lifecycle into the activity reports; failures are logged, never raised.
    void sync_reporting(const std::vector<json>* parents = nullptr, const json* parameters = nullptr);
    bool report(const char* method, const std::function<void(reporting::SessionReporter&)>& call);

    SmartSellingConfig config_;
    MetasProvider metas_;
    SnapshotsProvider snapshots_;
    PublishCommand publish_;
    OwnsAccount fast_owns_account_;
    Clock clock_;
    Clock wall_clock_;
    reporting::SessionReporter* reporting_;
    std::string trading_mode_;
    std::set<std::string> reported_sessions_;
    std::set<std::string> reported_terminal_;
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
