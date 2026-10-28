#include "algo/fast_trading/fast_trading.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "orders/commands.h"

namespace ng::fast {
namespace {

constexpr const char* kLog = "trading-ui.fast-trading";

std::optional<double> finite(const json* v) { return json_finite_double(v); }

double positive_float(const json* value, const std::string& label) {
    auto parsed = finite(value);
    if (!parsed || *parsed <= 0) throw std::invalid_argument(label + " must be a positive finite number");
    return *parsed;
}

int positive_int(const json* value, const std::string& label) {
    if (!value || value->is_boolean() || value->is_null()) throw std::invalid_argument(label + " must be a positive integer");
    std::optional<long long> parsed;
    if (value->is_number_integer()) parsed = value->get<long long>();
    else if (value->is_number_float()) {
        double d = value->get<double>();
        if (std::isfinite(d) && std::floor(d) == d) parsed = static_cast<long long>(d);
    } else if (value->is_string()) {
        std::string s = trim(value->get<std::string>());
        try {
            std::size_t idx = 0;
            long long v = std::stoll(s, &idx);
            if (idx == s.size()) parsed = v;
            else if (s.substr(idx) == ".0") parsed = v;
        } catch (...) {
        }
    }
    if (!parsed || *parsed <= 0) throw std::invalid_argument(label + " must be a positive integer");
    return static_cast<int>(*parsed);
}

int selected_int(const json* value, const std::vector<int>& choices, const std::string& label) {
    int parsed = positive_int(value, label);
    if (std::find(choices.begin(), choices.end(), parsed) == choices.end()) {
        std::vector<std::string> names;
        for (int c : choices) names.push_back(std::to_string(c));
        throw std::invalid_argument(label + " must be one of " + join(names, ", "));
    }
    return parsed;
}

const std::vector<int>& book_level_choices() {
    static const std::vector<int> v = {1, 2, 3, 4, 5};
    return v;
}
const std::vector<int>& target_percentage_choices() {
    static const std::vector<int> v = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    return v;
}
const std::vector<int>& cycle_seconds_choices() {
    static const std::vector<int> v = {5, 10, 15, 30, 60};
    return v;
}

double usd_cash(const AccountSnapshot& snapshot) {
    double value = 0.0;
    auto it = snapshot.available_cash_by_currency.find("USD");
    if (it != snapshot.available_cash_by_currency.end()) value = std::isfinite(it->second) ? it->second : 0.0;
    else {
        auto cit = snapshot.cash_by_currency.find("USD");
        if (cit != snapshot.cash_by_currency.end()) value = std::isfinite(cit->second) ? cit->second : 0.0;
        else value = std::isfinite(snapshot.cash) ? snapshot.cash : 0.0;
    }
    return std::max(0.0, value);
}

long long symbol_position_qty(const AccountSnapshot& snapshot, const std::string& symbol) {
    std::string target = normalize_symbol(symbol);
    double qty = 0.0;
    for (const auto& position : snapshot.positions) {
        if (normalize_symbol(position.symbol) != target) continue;
        double value = position.available_qty.has_value() ? *position.available_qty : position.qty;
        if (std::isfinite(value)) qty += value;
    }
    return static_cast<long long>(std::floor(std::max(0.0, qty)));
}

long long account_capacity(const std::string& account_id, const std::string& mode, const std::string& symbol,
                           double limit_price, const std::map<std::string, AccountSnapshot>* snapshots) {
    if (!snapshots) return 1000000000000000LL;
    auto it = snapshots->find(account_id);
    if (it == snapshots->end()) return 0;
    if (mode == "E") return static_cast<long long>(std::floor(usd_cash(it->second) / limit_price));
    return symbol_position_qty(it->second, symbol);
}

std::vector<BookLevel> ordered_top_levels(const std::vector<BookLevel>& levels, const std::string& mode, int book_levels) {
    std::vector<BookLevel> valid;
    for (const auto& level : levels) {
        if (!level.price || !std::isfinite(*level.price) || *level.price <= 0) continue;
        if (!level.quantity || !std::isfinite(*level.quantity) || *level.quantity <= 0) continue;
        valid.push_back(level);
    }
    std::stable_sort(valid.begin(), valid.end(), [&](const BookLevel& a, const BookLevel& b) {
        return mode == "F" ? *a.price > *b.price : *a.price < *b.price;
    });
    if (static_cast<int>(valid.size()) > book_levels) valid.resize(static_cast<std::size_t>(book_levels));
    return valid;
}

struct BookQuantity {
    double total = 0.0;
    std::optional<double> execution_limit;
    std::string reason;
};

BookQuantity book_quantity_and_limit(const std::string& mode, const std::vector<BookLevel>& levels, double price_limit,
                                     int book_levels) {
    auto top = ordered_top_levels(levels, mode, book_levels);
    if (top.empty()) return {0, std::nullopt, "no_resting_orders"};
    double extreme = *top[0].price;
    for (auto& l : top) extreme = mode == "E" ? std::max(extreme, *l.price) : std::min(extreme, *l.price);
    double execution_limit = mode == "E" ? std::min(extreme, price_limit) : std::max(extreme, price_limit);
    double total = 0.0;
    for (auto& l : top) {
        bool in_range = mode == "E" ? *l.price <= price_limit : *l.price >= price_limit;
        if (in_range && l.quantity && *l.quantity > 0) total += *l.quantity;
    }
    if (static_cast<long long>(std::floor(total)) <= 0) return {0, execution_limit, "no_resting_orders_within_price_limit"};
    return {total, execution_limit, ""};
}

std::vector<std::string> cooldown_eligible_accounts(const std::vector<std::string>& account_ids,
                                                    const AccountDirectory& metas,
                                                    const std::map<std::string, double>& minimum_cycles,
                                                    const std::map<std::string, double>* last_actions, double now) {
    std::vector<std::string> eligible;
    for (const auto& account_id : account_ids) {
        const AccountMeta* meta = metas.find(account_id);
        if (!meta) continue;
        std::string medium = to_upper(trim(meta->trading_medium.empty() ? "API" : meta->trading_medium));
        auto it = minimum_cycles.find(medium);
        double minimum = it != minimum_cycles.end() ? it->second : minimum_cycles.at("API");
        std::optional<double> last;
        if (last_actions) {
            auto lit = last_actions->find(account_id);
            if (lit != last_actions->end() && std::isfinite(lit->second)) last = lit->second;
        }
        if (!last || now - *last >= minimum) eligible.push_back(account_id);
    }
    return eligible;
}

struct Allocation {
    std::map<std::string, long long> per_account;  // insertion-ordered semantics restored below
    std::vector<std::string> order;
    bool capacity_noop = false;
    std::string reason;
};

Allocation even_capacity_allocation(long long total_qty, const std::vector<std::string>& account_ids, const std::string& mode,
                                    const std::string& symbol, double limit_price,
                                    const std::map<std::string, AccountSnapshot>* snapshots) {
    Allocation result;
    std::vector<std::string> candidates = account_ids;
    if (candidates.empty()) {
        result.reason = "no_accounts_off_cooldown";
        return result;
    }
    while (!candidates.empty()) {
        long long per_account = total_qty / static_cast<long long>(candidates.size());
        std::vector<std::string> underfunded;
        for (const auto& id : candidates) {
            if (account_capacity(id, mode, symbol, limit_price, snapshots) < per_account) underfunded.push_back(id);
        }
        if (underfunded.empty()) {
            if (per_account < kMinExecutableShares) {
                result.reason = "per_account_quantity_below_minimum";
                return result;
            }
            for (const auto& id : candidates) {
                result.per_account[id] = per_account;
                result.order.push_back(id);
            }
            return result;
        }
        std::vector<std::string> remaining;
        for (const auto& id : candidates) {
            if (std::find(underfunded.begin(), underfunded.end(), id) == underfunded.end()) remaining.push_back(id);
        }
        if (remaining.empty() && per_account < kMinExecutableShares) {
            result.reason = "per_account_quantity_below_minimum";
            return result;
        }
        candidates = remaining;
    }
    result.capacity_noop = true;
    result.reason = "no_account_capacity";
    return result;
}

std::string command_json(const json& command) { return json_dump_sorted(command); }

}  // namespace

std::string normalize_symbol(const std::string& value) { return canonical_symbol(value); }

std::vector<std::string> deduplicated_account_ids(const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& raw : ids) {
        std::string id = trim(raw);
        if (!id.empty() && !seen.count(id)) {
            out.push_back(id);
            seen.insert(id);
        }
    }
    return out;
}

RuntimeSettings RuntimeSettings::defaults() {
    RuntimeSettings s;
    s.aggression_levels = {{1, {2, 30.0}}, {2, {3, 20.0}}, {3, {5, 10.0}}};
    s.minimum_cycle_seconds_by_medium = {{"API", 3.0}, {"WEB", 30.0}, {"WINDOWS", 40.0}, {"EMULATOR", 40.0}};
    return s;
}

json CycleResult::to_json() const {
    json out;
    out["trading_mode"] = trading_mode;
    out["symbol"] = symbol;
    out["resting_side"] = resting_side;
    out["order_side"] = order_side;
    out["book_levels"] = book_levels;
    out["target_percentage"] = target_percentage;
    out["total_resting_qty"] = total_resting_qty;
    out["target_quantity"] = target_quantity;
    out["limit_price"] = limit_price ? json(*limit_price) : json(nullptr);
    out["per_account_qty"] = per_account_qty;
    out["allocated_quantity"] = allocated_quantity;
    out["cooldown_eligible_account_ids"] = cooldown_eligible_account_ids;
    out["capacity_eligible_account_ids"] = capacity_eligible_account_ids;
    out["commands"] = static_cast<long long>(commands.size());
    out["capacity_noop"] = capacity_noop;
    out["reason"] = reason;
    return out;
}

CycleResult build_fast_trading_cycle_commands(const CycleInputs& in) {
    std::string mode = to_upper(trim(in.trading_mode));
    std::string symbol = to_upper(trim(in.symbol));
    if (mode != "E" && mode != "F") throw std::invalid_argument("fast trading mode must be E or F");
    if (symbol.empty()) throw std::invalid_argument("symbol is required");
    if (!std::isfinite(in.price_limit) || in.price_limit <= 0) {
        throw std::invalid_argument("fast trading price limit must be a positive finite number");
    }
    json book_levels_json = in.book_levels;
    json target_pct_json = in.target_percentage;
    int book_levels = selected_int(&book_levels_json, book_level_choices(), "fast trading book levels");
    int target_percentage = selected_int(&target_pct_json, target_percentage_choices(), "fast trading target percentage");
    std::vector<std::string> account_ids = deduplicated_account_ids(in.account_ids);
    if (account_ids.empty()) throw std::invalid_argument("fast trading account_ids must be non-empty");
    if (!in.account_metas) throw std::invalid_argument("account metadata is required");
    for (const auto& id : account_ids) {
        if (!in.account_metas->contains(id)) throw std::invalid_argument("unknown fast trading account: " + id);
    }
    CycleResult result;
    result.trading_mode = mode;
    result.symbol = symbol;
    result.resting_side = mode == "E" ? "ASK" : "BID";
    result.order_side = mode == "E" ? "BUY" : "SELL";
    result.book_levels = book_levels;
    result.target_percentage = target_percentage;
    if (in.book.error) {
        result.reason = *in.book.error;
        return result;
    }
    const auto& levels = mode == "E" ? in.book.asks : in.book.bids;
    BookQuantity bq = book_quantity_and_limit(mode, levels, in.price_limit, book_levels);
    long long total_qty = static_cast<long long>(std::floor(bq.total));
    result.total_resting_qty = total_qty;
    result.target_quantity = static_cast<long long>(std::floor(bq.total * target_percentage / 100.0));
    result.limit_price = bq.execution_limit;
    if (!bq.reason.empty()) {
        result.reason = bq.reason;
        return result;
    }
    std::map<std::string, double> minimum_cycles = RuntimeSettings::defaults().minimum_cycle_seconds_by_medium;
    if (in.minimum_cycle_seconds_by_medium) {
        for (auto& [medium, seconds] : *in.minimum_cycle_seconds_by_medium) {
            if (!std::isfinite(seconds) || seconds <= 0) {
                throw std::invalid_argument(to_upper(trim(medium)) + " minimum cycle time must be a positive finite number");
            }
            minimum_cycles[to_upper(trim(medium))] = seconds;
        }
    }
    double now = in.now_monotonic ? *in.now_monotonic : mono_now();
    result.cooldown_eligible_account_ids =
        cooldown_eligible_accounts(account_ids, *in.account_metas, minimum_cycles, in.last_action_monotonic_by_account, now);
    if (result.cooldown_eligible_account_ids.empty()) {
        result.reason = "no_accounts_off_cooldown";
        return result;
    }
    std::vector<std::string> allocation_ids = result.cooldown_eligible_account_ids;
    std::vector<std::string> missing_snapshot_ids;
    if (in.account_snapshots) {
        allocation_ids.clear();
        for (const auto& id : result.cooldown_eligible_account_ids) {
            if (in.account_snapshots->count(id)) allocation_ids.push_back(id);
            else missing_snapshot_ids.push_back(id);
        }
        if (allocation_ids.empty()) {
            result.reason = "account_snapshots_unavailable";
            return result;
        }
    }
    Allocation allocation = even_capacity_allocation(result.target_quantity, allocation_ids, mode, symbol, *bq.execution_limit,
                                                     in.account_snapshots);
    if (allocation.capacity_noop && !missing_snapshot_ids.empty()) {
        result.reason = "account_snapshots_incomplete";
        return result;
    }
    result.capacity_noop = allocation.capacity_noop;
    if (!allocation.reason.empty()) {
        result.reason = allocation.reason;
        return result;
    }
    result.capacity_eligible_account_ids = allocation.order;
    result.per_account_qty = allocation.per_account.at(allocation.order.front());
    for (auto& [id, qty] : allocation.per_account) result.allocated_quantity += qty;
    std::string cycle_id = in.cycle_id.value_or("fast_" + mode + "_" + symbol + "_" + std::to_string(epoch_micros()));
    for (const auto& account_id : allocation.order) {
        json cmd = orders::build_limit_order_fok_command(account_id, symbol, result.order_side, allocation.per_account.at(account_id),
                                                         *bq.execution_limit, *in.account_metas);
        cmd["trading_mode"] = mode;
        cmd["algo_cycle_id"] = cycle_id;
        cmd["fast_trading_resting_side"] = result.resting_side;
        cmd["fast_trading_total_resting_qty"] = total_qty;
        cmd["fast_trading_target_percentage"] = target_percentage;
        cmd["fast_trading_target_qty"] = result.target_quantity;
        cmd["fast_trading_book_levels"] = book_levels;
        cmd["fast_trading_price_limit"] = in.price_limit;
        result.commands.push_back(std::move(cmd));
    }
    return result;
}

// ---------------------------------------------------------------------------
// StrategyManager
// ---------------------------------------------------------------------------

StrategyManager::StrategyManager(MarketDataStore& market_data, MetasProvider metas, PublishCommand publish,
                                 SnapshotsProvider snapshots, OperationsLog* operations_log, const RuntimeSettings& settings,
                                 Clock monotonic)
    : market_data_(market_data),
      metas_(std::move(metas)),
      snapshots_(std::move(snapshots)),
      publish_(std::move(publish)),
      operations_log_(operations_log),
      settings_(settings),
      monotonic_(monotonic ? std::move(monotonic) : Clock(&mono_now)) {
    for (int level : {1, 2, 3}) {
        if (!settings_.aggression_levels.count(level)) {
            throw std::invalid_argument("fast trading aggression level " + std::to_string(level) + " is missing");
        }
        auto [book_levels, cycle] = settings_.aggression_levels.at(level);
        if (book_levels <= 0 || book_levels > kMaxBookLevels || !std::isfinite(cycle) || cycle <= 0) {
            throw std::invalid_argument("aggression level " + std::to_string(level) + " is invalid");
        }
    }
    for (const char* medium : {"API", "WEB", "WINDOWS", "EMULATOR"}) {
        auto it = settings_.minimum_cycle_seconds_by_medium.find(medium);
        if (it == settings_.minimum_cycle_seconds_by_medium.end() || !std::isfinite(it->second) || it->second <= 0) {
            throw std::invalid_argument(std::string(medium) + " minimum cycle time must be a positive finite number");
        }
    }
}

StrategyManager::~StrategyManager() {
    stop_all();
    std::vector<std::shared_ptr<Strategy>> remaining;
    {
        std::lock_guard lock(mutex_);
        for (auto& [k, s] : strategies_) remaining.push_back(s);
        for (auto& [k, s] : stopping_strategies_) remaining.push_back(s);
        strategies_.clear();
        stopping_strategies_.clear();
    }
    for (auto& s : remaining) {
        request_stop(*s);
        if (s->thread.joinable()) s->thread.join();
    }
}

std::string StrategyManager::strategy_key(const std::string& trading_mode, const std::string& symbol) {
    return to_upper(trim(trading_mode)) + ":" + to_upper(trim(symbol));
}

std::set<std::string> StrategyManager::strategy_accounts(const json& command) {
    const json* raw = json_find(command, "fast_trading_account_ids");
    if (!raw || !raw->is_array()) return {};
    std::vector<std::string> ids;
    for (const auto& v : *raw) ids.push_back(json_to_string(&v).value_or(""));
    auto dedup = deduplicated_account_ids(ids);
    return std::set<std::string>(dedup.begin(), dedup.end());
}

bool StrategyManager::has_strategy(const std::string& account_id, const std::string& symbol) const {
    std::string target = normalize_symbol(symbol);
    std::lock_guard lock(mutex_);
    auto matches = [&](const std::shared_ptr<Strategy>& s) {
        return normalize_symbol(json_to_string(json_find(s->command, "symbol")).value_or("")) == target &&
               strategy_accounts(s->command).count(account_id);
    };
    for (auto& [k, s] : strategies_) if (matches(s)) return true;
    for (auto& [k, s] : stopping_strategies_) if (matches(s)) return true;
    return inflight_account_ids_.count(account_id) > 0;
}

bool StrategyManager::has_active_session() const {
    std::lock_guard lock(mutex_);
    return !strategies_.empty() || !stopping_strategies_.empty() || !inflight_account_ids_.empty();
}

bool StrategyManager::owns_account(const std::string& account_id) const {
    std::lock_guard lock(mutex_);
    if (inflight_account_ids_.count(account_id)) return true;
    for (auto& [k, s] : strategies_) if (strategy_accounts(s->command).count(account_id)) return true;
    for (auto& [k, s] : stopping_strategies_) if (strategy_accounts(s->command).count(account_id)) return true;
    return false;
}

bool StrategyManager::has_running_strategy(const std::string& account_id, const std::string& symbol) const {
    std::lock_guard lock(mutex_);
    for (auto& [k, s] : strategies_) {
        if (!s->stop_requested.load() &&
            normalize_symbol(json_to_string(json_find(s->command, "symbol")).value_or("")) == normalize_symbol(symbol) &&
            strategy_accounts(s->command).count(account_id)) {
            return true;
        }
    }
    return false;
}

StrategyManager::ValidatedValues StrategyManager::validated_strategy_values(const json& command) const {
    ValidatedValues v;
    v.mode = to_upper(trim(json_to_string(json_find(command, "trading_mode")).value_or("")));
    v.symbol = to_upper(trim(json_to_string(json_find(command, "symbol")).value_or("")));
    if (v.mode != "E" && v.mode != "F") throw std::invalid_argument("fast trading strategy mode must be E or F");
    if (v.symbol.empty()) throw std::invalid_argument("fast trading strategy requires a symbol");
    v.price_limit = positive_float(json_find(command, "fast_trading_price_limit"), "fast trading price limit");
    const json* raw_accounts = json_find(command, "fast_trading_account_ids");
    if (!raw_accounts || !raw_accounts->is_array()) throw std::invalid_argument("fast trading strategy requires account ids");
    std::vector<std::string> ids;
    for (const auto& a : *raw_accounts) ids.push_back(json_to_string(&a).value_or(""));
    v.account_ids = deduplicated_account_ids(ids);
    if (v.account_ids.empty()) throw std::invalid_argument("fast trading strategy requires account ids");
    v.book_levels = selected_int(json_find(command, "fast_trading_book_levels"), book_level_choices(), "fast trading book levels");
    v.target_percentage = selected_int(json_find(command, "fast_trading_target_percentage"), target_percentage_choices(),
                                       "fast trading target percentage");
    v.cycle_seconds = selected_int(json_find(command, "fast_trading_cycle_seconds"), cycle_seconds_choices(),
                                   "fast trading cycle seconds");
    return v;
}

std::string StrategyManager::start_strategy(const json& start_command) {
    ValidatedValues v = validated_strategy_values(start_command);
    std::string key = strategy_key(v.mode, v.symbol);
    auto strategy = std::make_shared<Strategy>();
    strategy->key = key;
    strategy->command = start_command;
    strategy->started_at = iso_utc_now();
    {
        std::lock_guard lock(mutex_);
        if (has_active_session()) throw std::invalid_argument("Stop the existing algorithm session before starting another");
        strategies_[key] = strategy;
        record_started(*strategy);
        try {
            strategy->thread = std::thread([this, strategy] { run_strategy(strategy); });
        } catch (...) {
            strategies_.erase(key);
            throw;
        }
    }
    NG_LOG_INFO(kLog,
                "Algo trading started successfully mode={} symbol={} key={} command_id={} book_levels={} target_percentage={} "
                "cycle_seconds={} test_mode={}",
                v.mode, v.symbol, key, json_to_string(json_find(start_command, "command_id")).value_or(""), v.book_levels,
                v.target_percentage, v.cycle_seconds, json_truthy(json_find(start_command, "fast_trading_test_mode")));
    return key;
}

int StrategyManager::stop_strategy(const std::string& trading_mode, const std::string& symbol) {
    return stop_keys({strategy_key(trading_mode, symbol)}, kStopReasonManual);
}

int StrategyManager::stop_matching(const std::string& trading_mode, const std::vector<std::string>& account_ids) {
    std::string mode = to_upper(trim(trading_mode));
    auto filter_vec = deduplicated_account_ids(account_ids);
    std::set<std::string> filter(filter_vec.begin(), filter_vec.end());
    std::vector<std::string> keys;
    {
        std::lock_guard lock(mutex_);
        for (auto& [key, s] : strategies_) {
            if (to_upper(trim(json_to_string(json_find(s->command, "trading_mode")).value_or(""))) != mode) continue;
            if (!filter.empty()) {
                auto accounts = strategy_accounts(s->command);
                bool intersects = false;
                for (auto& a : accounts) if (filter.count(a)) intersects = true;
                if (!intersects) continue;
            }
            keys.push_back(key);
        }
    }
    return stop_keys(keys, kStopReasonManual);
}

int StrategyManager::stop_all() {
    std::vector<std::string> keys;
    {
        std::lock_guard lock(mutex_);
        for (auto& [key, s] : strategies_) keys.push_back(key);
    }
    return stop_keys(keys, kStopReasonManual);
}

void StrategyManager::request_stop(Strategy& strategy) {
    {
        std::lock_guard lock(strategy.wait_mutex);
        strategy.stop_requested.store(true);
    }
    strategy.wait_cv.notify_all();
}

int StrategyManager::stop_keys(const std::vector<std::string>& keys, const std::string& reason) {
    std::vector<std::shared_ptr<Strategy>> stopped;
    {
        std::lock_guard lock(mutex_);
        for (const auto& key : keys) {
            auto it = strategies_.find(key);
            if (it == strategies_.end()) continue;
            auto strategy = it->second;
            strategies_.erase(it);
            strategy->last_reason = reason;
            request_stop(*strategy);
            stopping_strategies_[key] = strategy;
            stopped.push_back(strategy);
        }
    }
    for (auto& strategy : stopped) {
        if (strategy->thread.joinable() && strategy->thread.get_id() != std::this_thread::get_id()) {
            // Wait up to two seconds for the worker to exit, like the Python join(timeout=2.0).
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                {
                    std::lock_guard lock(mutex_);
                    if (!stopping_strategies_.count(strategy->key)) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        {
            std::lock_guard lock(mutex_);
            // The worker removes itself from stopping_strategies_ when it exits; if it already did, join it.
            if (!stopping_strategies_.count(strategy->key) && strategy->thread.joinable() &&
                strategy->thread.get_id() != std::this_thread::get_id()) {
                strategy->thread.join();
            }
        }
        NG_LOG_INFO(kLog, "Algo trading stopped mode={} symbol={} key={} command_id={} reason={}",
                    json_to_string(json_find(strategy->command, "trading_mode")).value_or(""),
                    json_to_string(json_find(strategy->command, "symbol")).value_or(""), strategy->key,
                    json_to_string(json_find(strategy->command, "command_id")).value_or(""), reason);
    }
    return static_cast<int>(stopped.size());
}

void StrategyManager::record_started(const Strategy& strategy) {
    if (!operations_log_) return;
    try {
        operations_log_->record_algo_started(strategy.command, strategy.key,
                                             json_to_string(json_find(strategy.command, "command_id")));
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed recording algo start operations event key={}: {}", strategy.key, e.what());
    }
}

void StrategyManager::record_ended(const Strategy& strategy, const std::string& reason, long long cycles, long long published) {
    if (!operations_log_) return;
    try {
        operations_log_->record_algo_ended(strategy.command, strategy.key,
                                           json_to_string(json_find(strategy.command, "command_id")), reason, cycles, published);
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed recording algo end operations event key={}: {}", strategy.key, e.what());
    }
}

void StrategyManager::run_strategy(std::shared_ptr<Strategy> strategy) {
    try {
        ValidatedValues v = validated_strategy_values(strategy->command);
        while (!strategy->stop_requested.load()) {
            double cycle_start = monotonic_();
            CycleResult result = run_cycle(strategy->command);
            bool test_mode = json_truthy(json_find(strategy->command, "fast_trading_test_mode"));
            long long published_count = test_mode ? 0 : static_cast<long long>(result.commands.size());
            bool reached_noop_limit = false;
            {
                std::lock_guard lock(mutex_);
                strategy->cycles += 1;
                strategy->published_commands += published_count;
                strategy->last_cycle = result.to_json();
                if (!result.commands.empty()) strategy->consecutive_capacity_noops = 0;
                else if (result.capacity_noop) strategy->consecutive_capacity_noops += 1;
                if (!strategy->stop_requested.load()) {
                    if (strategy->consecutive_capacity_noops >= kMaxConsecutiveCapacityNoops) {
                        strategy->last_reason = kStopReasonCapacityNoops;
                        reached_noop_limit = true;
                    } else {
                        strategy->last_reason = result.reason;
                    }
                }
            }
            if (reached_noop_limit) {
                NG_LOG_INFO(kLog, "Algo trading automatically stopped mode={} symbol={} key={} command_id={} reason={}",
                            json_to_string(json_find(strategy->command, "trading_mode")).value_or(""),
                            json_to_string(json_find(strategy->command, "symbol")).value_or(""), strategy->key,
                            json_to_string(json_find(strategy->command, "command_id")).value_or(""), kStopReasonCapacityNoops);
                break;
            }
            double elapsed = monotonic_() - cycle_start;
            double wait_seconds = std::max(0.0, static_cast<double>(v.cycle_seconds) - elapsed);
            std::unique_lock lock(strategy->wait_mutex);
            bool stopped = strategy->wait_cv.wait_for(lock, std::chrono::duration<double>(wait_seconds),
                                                      [&] { return strategy->stop_requested.load(); });
            if (stopped) break;
        }
    } catch (const std::exception& e) {
        {
            std::lock_guard lock(mutex_);
            if (!strategy->stop_requested.load()) strategy->last_reason = "Strategy error";
        }
        NG_LOG_ERROR(kLog, "Fast trading strategy failed key={}: {}", strategy->key, e.what());
    }
    std::string reason;
    long long cycles, published;
    {
        std::lock_guard lock(mutex_);
        auto it = strategies_.find(strategy->key);
        if (it != strategies_.end() && it->second == strategy) strategies_.erase(it);
        auto sit = stopping_strategies_.find(strategy->key);
        if (sit != stopping_strategies_.end() && sit->second == strategy) stopping_strategies_.erase(sit);
        reason = strategy->last_reason.empty() ? "Strategy stopped" : strategy->last_reason;
        cycles = strategy->cycles;
        published = strategy->published_commands;
        strategy->thread.detach();  // the manager no longer needs to join this worker
    }
    record_ended(*strategy, reason, cycles, published);
}

CycleResult StrategyManager::run_cycle(const json& command) {
    ValidatedValues v = validated_strategy_values(command);
    OrderBookSnapshot book = market_data_.get_book(v.symbol, v.book_levels);
    const AccountDirectory& metas = metas_();
    std::optional<std::map<std::string, AccountSnapshot>> snapshots;
    if (snapshots_) snapshots = snapshots_();
    double now = monotonic_();
    CycleResult result;
    std::set<std::string> reserved;
    {
        std::lock_guard lock(mutex_);
        std::map<std::string, double> effective = last_action_monotonic_by_account_;
        for (const auto& id : inflight_account_ids_) effective[id] = now;
        CycleInputs inputs;
        inputs.trading_mode = v.mode;
        inputs.symbol = v.symbol;
        inputs.book = book;
        inputs.price_limit = v.price_limit;
        inputs.account_ids = v.account_ids;
        inputs.book_levels = v.book_levels;
        inputs.target_percentage = v.target_percentage;
        inputs.account_metas = &metas;
        inputs.account_snapshots = snapshots ? &*snapshots : nullptr;
        inputs.minimum_cycle_seconds_by_medium = &settings_.minimum_cycle_seconds_by_medium;
        inputs.last_action_monotonic_by_account = &effective;
        inputs.now_monotonic = now;
        result = build_fast_trading_cycle_commands(inputs);
        for (const auto& cmd : result.commands) {
            std::string id = trim(json_to_string(json_find(cmd, "account_id")).value_or(""));
            if (!id.empty()) reserved.insert(id);
        }
        inflight_account_ids_.insert(reserved.begin(), reserved.end());
    }
    bool test_mode = json_truthy(json_find(command, "fast_trading_test_mode"));
    try {
        for (auto& cmd : result.commands) {
            cmd["fast_trading_cycle_seconds"] = v.cycle_seconds;
            std::string account_id = trim(json_to_string(json_find(cmd, "account_id")).value_or(""));
            std::string key = account_id.empty() ? "LIMIT_ORDER_FOK" : account_id;
            if (test_mode) {
                cmd["fast_trading_test_mode"] = true;
                NG_LOG_INFO(kLog, "Fast trading TEST MODE logged command without Kafka publish key={} command={}", key,
                            command_json(cmd));
            } else {
                NG_LOG_INFO(kLog, "Publishing fast trading command key={} command={}", key, command_json(cmd));
                publish_(cmd, key);
            }
            if (!account_id.empty()) {
                std::lock_guard lock(mutex_);
                last_action_monotonic_by_account_[account_id] = monotonic_();
                inflight_account_ids_.erase(account_id);
            }
        }
    } catch (...) {
        std::lock_guard lock(mutex_);
        for (const auto& id : reserved) inflight_account_ids_.erase(id);
        throw;
    }
    {
        std::lock_guard lock(mutex_);
        for (const auto& id : reserved) inflight_account_ids_.erase(id);
    }
    NG_LOG_INFO(kLog,
                "Fast trading cycle mode={} symbol={} resting_side={} book_levels={} target_percentage={} total_qty={} "
                "target_qty={} allocated_qty={} price={} commands={} capacity_noop={} reason={}",
                result.trading_mode, result.symbol, result.resting_side, v.book_levels, v.target_percentage,
                result.total_resting_qty, result.target_quantity, result.allocated_quantity,
                result.limit_price ? format_double(*result.limit_price) : "None", result.commands.size(), result.capacity_noop,
                result.reason);
    return result;
}

std::vector<StrategyStatus> StrategyManager::statuses() const {
    std::lock_guard lock(mutex_);
    std::vector<StrategyStatus> out;
    auto add = [&](const std::shared_ptr<Strategy>& s, bool stopping) {
        StrategyStatus st;
        st.key = s->key;
        st.command = s->command;
        st.started_at = s->started_at;
        st.cycles = s->cycles;
        st.published_commands = s->published_commands;
        st.consecutive_capacity_noops = s->consecutive_capacity_noops;
        st.last_reason = s->last_reason;
        st.stopping = stopping || s->stop_requested.load();
        st.last_cycle = s->last_cycle;
        out.push_back(std::move(st));
    };
    for (auto& [k, s] : strategies_) add(s, false);
    for (auto& [k, s] : stopping_strategies_) add(s, true);
    return out;
}

}  // namespace ng::fast
