// Fast trading modes E (buy up to an upper limit) and F (sell down to a lower limit):
// pure cycle construction plus the threaded strategy manager (services/fast_trading.py).
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"
#include "model/account.h"
#include "model/market_data.h"
#include "ops/operations_log.h"

namespace ng::fast {

inline constexpr int kMinExecutableShares = 100;
inline constexpr int kMaxBookLevels = 20;
inline constexpr int kMaxConsecutiveCapacityNoops = 10;
inline constexpr const char* kStopReasonManual = "Manual stop";
inline constexpr const char* kStopReasonCapacityNoops = "10 consecutive purchasing-power no-ops";

struct RuntimeSettings {
    std::map<int, std::pair<int, double>> aggression_levels;  // level -> (book_levels, cycle_seconds)
    std::map<std::string, double> minimum_cycle_seconds_by_medium;
    static RuntimeSettings defaults();
};

struct CycleResult {
    std::string trading_mode;
    std::string symbol;
    std::string resting_side;
    std::string order_side;
    int book_levels = 0;
    int target_percentage = 0;
    long long total_resting_qty = 0;
    long long target_quantity = 0;
    std::optional<double> limit_price;
    long long per_account_qty = 0;
    long long allocated_quantity = 0;
    std::vector<std::string> cooldown_eligible_account_ids;
    std::vector<std::string> capacity_eligible_account_ids;
    std::vector<json> commands;
    bool capacity_noop = false;
    std::string reason;
    json to_json() const;
};

struct CycleInputs {
    std::string trading_mode;
    std::string symbol;
    OrderBookSnapshot book;
    double price_limit = 0.0;
    std::vector<std::string> account_ids;
    int book_levels = 0;
    int target_percentage = 0;
    const AccountDirectory* account_metas = nullptr;
    const std::map<std::string, AccountSnapshot>* account_snapshots = nullptr;  // nullptr: unlimited capacity (tests)
    const std::map<std::string, double>* minimum_cycle_seconds_by_medium = nullptr;
    const std::map<std::string, double>* last_action_monotonic_by_account = nullptr;
    std::optional<double> now_monotonic;
    std::optional<std::string> cycle_id;
};

// Throws std::invalid_argument for malformed inputs.
CycleResult build_fast_trading_cycle_commands(const CycleInputs& inputs);

struct StrategyStatus {
    std::string key;
    json command;
    std::string started_at;
    long long cycles = 0;
    long long published_commands = 0;
    int consecutive_capacity_noops = 0;
    std::string last_reason;
    bool stopping = false;
    json last_cycle;  // summary of the most recent cycle
};

class StrategyManager {
public:
    using MetasProvider = std::function<const AccountDirectory&()>;
    using SnapshotsProvider = std::function<std::map<std::string, AccountSnapshot>()>;
    using PublishCommand = std::function<void(const json& command, const std::string& key)>;
    using Clock = std::function<double()>;

    StrategyManager(MarketDataStore& market_data, MetasProvider metas, PublishCommand publish,
                    SnapshotsProvider snapshots = nullptr, OperationsLog* operations_log = nullptr,
                    const RuntimeSettings& settings = RuntimeSettings::defaults(), Clock monotonic = nullptr);
    ~StrategyManager();

    static std::string strategy_key(const std::string& trading_mode, const std::string& symbol);

    bool has_strategy(const std::string& account_id, const std::string& symbol) const;
    bool has_active_session() const;
    bool owns_account(const std::string& account_id) const;
    bool has_running_strategy(const std::string& account_id, const std::string& symbol) const;

    // Returns the strategy key. Throws std::invalid_argument / std::runtime_error.
    std::string start_strategy(const json& start_command);
    int stop_strategy(const std::string& trading_mode, const std::string& symbol);
    int stop_matching(const std::string& trading_mode, const std::vector<std::string>& account_ids);
    int stop_all();
    // Runs one cycle for `command` (publishing unless test mode); exposed for tests and status.
    CycleResult run_cycle(const json& command);
    std::vector<StrategyStatus> statuses() const;

private:
    struct Strategy {
        std::string key;
        json command;
        std::atomic<bool> stop_requested{false};
        std::thread thread;
        std::string started_at;
        long long cycles = 0;
        long long published_commands = 0;
        int consecutive_capacity_noops = 0;
        std::string last_reason;
        json last_cycle;
        std::mutex wait_mutex;
        std::condition_variable wait_cv;
    };
    struct ValidatedValues {
        std::string mode, symbol;
        double price_limit;
        std::vector<std::string> account_ids;
        int book_levels, target_percentage, cycle_seconds;
    };

    ValidatedValues validated_strategy_values(const json& command) const;
    static std::set<std::string> strategy_accounts(const json& command);
    int stop_keys(const std::vector<std::string>& keys, const std::string& reason);
    void run_strategy(std::shared_ptr<Strategy> strategy);
    void record_started(const Strategy& strategy);
    void record_ended(const Strategy& strategy, const std::string& reason, long long cycles, long long published);
    void request_stop(Strategy& strategy);

    MarketDataStore& market_data_;
    MetasProvider metas_;
    SnapshotsProvider snapshots_;
    PublishCommand publish_;
    OperationsLog* operations_log_;
    RuntimeSettings settings_;
    Clock monotonic_;
    mutable std::recursive_mutex mutex_;
    std::map<std::string, std::shared_ptr<Strategy>> strategies_;
    std::map<std::string, std::shared_ptr<Strategy>> stopping_strategies_;
    std::map<std::string, double> last_action_monotonic_by_account_;
    std::set<std::string> inflight_account_ids_;
};

std::string normalize_symbol(const std::string& value);
std::vector<std::string> deduplicated_account_ids(const std::vector<std::string>& ids);

}  // namespace ng::fast
