#include "algo/fast_trading/fast_trading.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/logging.h"
#include "core/strings.h"
#include "core/equity_price.h"
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
    static const std::vector<int> v(std::begin(orders::kFastTargetPercentageChoices), std::end(orders::kFastTargetPercentageChoices));
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

// Sum of actual holdings for every participant, independent of sellable quantity; nullopt when any
// participant has no snapshot or reports a non-finite quantity.
std::optional<double> total_position(const std::vector<std::string>& account_ids, const std::string& symbol,
                                     const std::map<std::string, AccountSnapshot>* snapshots) {
    if (!snapshots) return std::nullopt;
    std::string target = normalize_symbol(symbol);
    double total = 0.0;
    for (const auto& account_id : account_ids) {
        auto it = snapshots->find(account_id);
        if (it == snapshots->end()) return std::nullopt;
        for (const auto& position : it->second.positions) {
            if (normalize_symbol(position.symbol) != target) continue;
            if (!std::isfinite(position.qty)) return std::nullopt;
            total += position.qty;
        }
    }
    if (!std::isfinite(total)) return std::nullopt;
    return total;
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
                                     int book_levels, const Decimal& tick) {
    auto top = ordered_top_levels(levels, mode, book_levels);
    if (top.empty()) return {0, std::nullopt, "no_resting_orders"};
    double extreme = *top[0].price;
    for (auto& l : top) extreme = mode == "E" ? std::max(extreme, *l.price) : std::min(extreme, *l.price);
    Decimal bound = mode=="E" ? round_equity_price_down(Decimal::from_double(price_limit),tick)
                              : round_equity_price_up(Decimal::from_double(price_limit),tick);
    if (!bound.is_positive()) return {0, std::nullopt, "price_limit_below_minimum_tick"};
    Decimal execution = mode=="E" ? Decimal::min(round_equity_price_up(Decimal::from_double(extreme),tick),bound)
                                  : Decimal::max(round_equity_price_down(Decimal::from_double(extreme),tick),bound);
    double execution_limit = execution.to_double();
    double total = 0.0;
    for (auto& l : top) {
        bool in_range = mode == "E" ? *l.price <= execution_limit : *l.price >= execution_limit;
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
    out["total_position"] = total_position ? json(*total_position) : json(nullptr);
    out["total_target_position"] = total_target_position ? json(*total_target_position) : json(nullptr);
    out["target_position_reached"] = target_position_reached;
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
    if (in.total_target_position && (*in.total_target_position < 0 || *in.total_target_position > 9007199254740991LL)) {
        throw std::invalid_argument(orders::kFastTotalTargetPositionError);
    }
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
    result.total_target_position = in.total_target_position;
    if (in.total_target_position) {
        result.total_position = total_position(account_ids, symbol, in.account_snapshots);
        if (!result.total_position) {
            // An incomplete total must not trigger a stop or allow new orders.
            result.reason = "total_position_unavailable";
            return result;
        }
        double target = static_cast<double>(*in.total_target_position);
        result.target_position_reached = mode == "E" ? *result.total_position >= target : *result.total_position < target;
        if (result.target_position_reached) {
            result.reason = kStopReasonTargetPosition;
            return result;
        }
    }
    if (in.book.error) {
        result.reason = *in.book.error;
        return result;
    }
    const auto& levels = mode == "E" ? in.book.asks : in.book.bids;
    BookQuantity bq = book_quantity_and_limit(mode, levels, in.price_limit, book_levels, Decimal::parse(in.tick_size));
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
        if (in.total_target_position) cmd["fast_trading_total_target_position"] = *in.total_target_position;
        result.commands.push_back(std::move(cmd));
    }
    return result;
}

// ---------------------------------------------------------------------------
// StrategyManager
// ---------------------------------------------------------------------------

StrategyManager::StrategyManager(MarketDataStore& market_data, MetasProvider metas, PublishCommand publish,
                                 SnapshotsProvider snapshots, OperationsLog* operations_log, const RuntimeSettings& settings,
                                 Clock monotonic, reporting::SessionReporter* reporting, std::shared_ptr<algo::SessionCoordinator> session_coordinator)
    : market_data_(market_data),
      metas_(std::move(metas)),
      snapshots_(std::move(snapshots)),
      publish_(std::move(publish)),
      operations_log_(operations_log),
      settings_(settings),
      monotonic_(monotonic ? std::move(monotonic) : Clock(&mono_now)),
      reporting_(reporting),
      session_coordinator_(session_coordinator ? std::move(session_coordinator) : std::make_shared<algo::SessionCoordinator>()) {
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
    session_coordinator_->register_provider(this, [this] { return algo_sessions(); });
}

StrategyManager::~StrategyManager() {
    stop_all();
    std::vector<std::shared_ptr<Strategy>> remaining;
    {
        std::lock_guard lock(mutex_);
        for (auto& [k, s] : strategies_) remaining.push_back(s);
        for (auto& [k, s] : stopping_strategies_) remaining.push_back(s);
        for (auto& s : finished_) remaining.push_back(s);
        strategies_.clear();
        stopping_strategies_.clear();
        finished_.clear();
    }
    for (auto& s : remaining) {
        request_stop(*s);
        if (s->thread.joinable()) s->thread.join();
    }
    session_coordinator_->unregister_provider(this);
}

std::string StrategyManager::strategy_key(const std::string& trading_mode, const std::string& symbol) {
    return to_upper(trim(trading_mode)) + ":" + to_upper(trim(symbol));
}

std::set<std::string> StrategyManager::strategy_accounts(const json& command) {
    const json* raw = json_find(command, "fast_trading_account_ids");
    if (!raw || !raw->is_array()) return {};
    std::vector<std::string> ids;
    for (const auto& v : *raw) ids.push_back(json_python_truthy(&v) ? json_to_string(&v).value_or("") : "");
    auto dedup = deduplicated_account_ids(ids);
    return std::set<std::string>(dedup.begin(), dedup.end());
}

std::string StrategyManager::command_session_id(const json& command) {
    return json_to_string(json_find(command, "command_id")).value_or("");
}

void StrategyManager::report(const char* method, const std::function<void(reporting::SessionReporter&)>& call) const {
    if (!reporting_) return;
    try {
        call(*reporting_);
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed updating algorithm reporting method={}: {}", method, e.what());
    }
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

std::vector<algo::Session> StrategyManager::algo_sessions() const {
    std::lock_guard lock(mutex_);
    std::vector<algo::Session> sessions;
    for (const auto* group : {&strategies_, &stopping_strategies_}) {
        for (const auto& [id, strategy] : *group) {
            const auto accounts = strategy_accounts(strategy->command);
            sessions.push_back({id, to_upper(trim(strategy->command.at("trading_mode").get<std::string>())),
                                strategy->command.at("symbol").get<std::string>(), {accounts.begin(), accounts.end()}});
        }
    }
    return sessions;
}

bool StrategyManager::has_running_strategy(const std::string& account_id, const std::string& symbol) const {
    // Only a running worker may hand off a new order after a stop request.
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
    for (const auto& a : *raw_accounts) ids.push_back(json_python_truthy(&a) ? json_to_string(&a).value_or("") : "");
    v.account_ids = deduplicated_account_ids(ids);
    if (v.account_ids.empty()) throw std::invalid_argument("fast trading strategy requires account ids");
    v.book_levels = selected_int(json_find(command, "fast_trading_book_levels"), book_level_choices(), "fast trading book levels");
    v.target_percentage = selected_int(json_find(command, "fast_trading_target_percentage"), target_percentage_choices(),
                                       "fast trading target percentage");
    v.cycle_seconds = selected_int(json_find(command, "fast_trading_cycle_seconds"), cycle_seconds_choices(),
                                   "fast trading cycle seconds");
    const json* target = json_find(command, "fast_trading_total_target_position");
    v.total_target_position = orders::parse_total_target_position(target ? *target : json(nullptr));
    return v;
}

void StrategyManager::reap_finished() {
    std::vector<std::shared_ptr<Strategy>> finished;
    {
        std::lock_guard lock(mutex_);
        for (auto it = finished_.begin(); it != finished_.end();) {
            if ((*it)->thread.get_id() == std::this_thread::get_id()) {
                ++it;  // a worker cannot join itself; a later caller reaps it
            } else {
                finished.push_back(*it);
                it = finished_.erase(it);
            }
        }
    }
    for (auto& s : finished) {
        if (s->thread.joinable()) s->thread.join();
    }
}

std::string StrategyManager::start_strategy(const json& start_command) {
    reap_finished();
    ValidatedValues v = validated_strategy_values(start_command);
    json strategy_command = start_command;
    if (v.total_target_position) strategy_command["fast_trading_total_target_position"] = *v.total_target_position;
    // The start command id identifies the session here, in reports and in logs.
    if (!json_python_truthy(json_find(strategy_command, "command_id"))) strategy_command["command_id"] = uuid4_hex();
    std::string session_id = command_session_id(strategy_command);
    std::string key = strategy_key(v.mode, v.symbol);

    auto strategy = std::make_shared<Strategy>();
    strategy->key = key;
    strategy->session_id = session_id;
    strategy->command = strategy_command;
    strategy->started_at = iso_utc_now();
    {
        std::lock_guard admission(session_coordinator_->mutex);
        std::lock_guard lock(mutex_);
        if (strategies_.count(session_id) || stopping_strategies_.count(session_id)) {
            throw std::invalid_argument("An algorithm session with this command id is already active");
        }
        session_coordinator_->require_available(v.mode, v.symbol, v.account_ids);
        strategies_[session_id] = strategy;
        auto accounts = strategy_accounts(strategy->command);
        std::vector<std::string> sorted_accounts(accounts.begin(), accounts.end());
        try {
            if (reporting_) reporting_->start_session(session_id, strategy->command, sorted_accounts, strategy->started_at);
            record_started(*strategy);
            std::function<void()> body = [this, strategy] { run_strategy(strategy); };
            strategy->thread = worker_spawner_ ? worker_spawner_(std::move(body)) : std::thread(std::move(body));
        } catch (...) {
            strategies_.erase(session_id);
            report("end_session", [&](reporting::SessionReporter& r) {
                r.end_session(session_id, "failed", std::string("Strategy worker failed to start"));
            });
            throw;
        }
    }
    NG_LOG_INFO(kLog,
                "Algo trading started successfully mode={} symbol={} key={} command_id={} book_levels={} target_percentage={} "
                "cycle_seconds={} total_target_position={} test_mode={}",
                v.mode, v.symbol, key, session_id, v.book_levels, v.target_percentage, v.cycle_seconds,
                v.total_target_position ? std::to_string(*v.total_target_position) : "None",
                json_truthy(json_find(strategy_command, "fast_trading_test_mode")));
    return key;
}

int StrategyManager::stop_strategy(const std::string& trading_mode, const std::string& symbol) {
    std::string key = strategy_key(trading_mode, symbol);
    std::vector<std::string> session_ids;
    {
        std::lock_guard lock(mutex_);
        for (auto& [session_id, s] : strategies_) {
            if (s->key == key) session_ids.push_back(session_id);
        }
    }
    return stop_sessions(session_ids, kStopReasonManual);
}

int StrategyManager::stop_matching(const std::string& trading_mode, const std::vector<std::string>& account_ids) {
    std::string mode = to_upper(trim(trading_mode));
    auto filter_vec = deduplicated_account_ids(account_ids);
    std::set<std::string> filter(filter_vec.begin(), filter_vec.end());
    std::vector<std::string> session_ids;
    {
        std::lock_guard lock(mutex_);
        for (auto& [session_id, s] : strategies_) {
            if (to_upper(trim(json_to_string(json_find(s->command, "trading_mode")).value_or(""))) != mode) continue;
            if (!filter.empty()) {
                auto accounts = strategy_accounts(s->command);
                bool intersects = false;
                for (auto& a : accounts) if (filter.count(a)) intersects = true;
                if (!intersects) continue;
            }
            session_ids.push_back(session_id);
        }
    }
    return stop_sessions(session_ids, kStopReasonManual);
}

int StrategyManager::stop_all() {
    std::vector<std::string> session_ids;
    {
        std::lock_guard lock(mutex_);
        for (auto& [session_id, s] : strategies_) session_ids.push_back(session_id);
    }
    return stop_sessions(session_ids, kStopReasonManual);
}

int StrategyManager::stop_session(const std::string& session_id) {
    return stop_sessions({session_id}, kStopReasonManual);
}

void StrategyManager::request_stop(Strategy& strategy) {
    {
        std::lock_guard lock(strategy.wait_mutex);
        strategy.stop_requested.store(true);
    }
    strategy.wait_cv.notify_all();
}

int StrategyManager::stop_sessions(const std::vector<std::string>& session_ids, const std::string& reason) {
    std::vector<std::shared_ptr<Strategy>> stopped;
    {
        std::lock_guard lock(mutex_);
        for (const auto& session_id : session_ids) {
            auto it = strategies_.find(session_id);
            if (it == strategies_.end()) continue;
            auto strategy = it->second;
            strategies_.erase(it);
            strategy->last_reason = reason;
            request_stop(*strategy);
            stopping_strategies_[session_id] = strategy;
            report("heartbeat_session", [&](reporting::SessionReporter& r) { r.heartbeat_session(session_id, "stopping"); });
            stopped.push_back(strategy);
        }
    }
    for (auto& strategy : stopped) {
        if (strategy->thread.get_id() != std::this_thread::get_id()) {
            // Wait up to two seconds for the worker to finish, like the Python join(timeout=2.0). A worker
            // still blocked in a publish keeps its accounts reserved until its handoff returns.
            std::unique_lock lock(strategy->wait_mutex);
            strategy->wait_cv.wait_for(lock, std::chrono::seconds(2), [&] { return strategy->finished; });
        }
        NG_LOG_INFO(kLog, "Algo trading stopped mode={} symbol={} key={} command_id={} reason={}",
                    json_to_string(json_find(strategy->command, "trading_mode")).value_or(""),
                    json_to_string(json_find(strategy->command, "symbol")).value_or(""), strategy->key, strategy->session_id, reason);
    }
    reap_finished();
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

void StrategyManager::set_worker_spawner_for_test(WorkerSpawner spawner) {
    std::lock_guard lock(mutex_);
    worker_spawner_ = std::move(spawner);
}

void StrategyManager::set_cycle_waiter_for_test(CycleWaiter waiter) {
    std::lock_guard lock(mutex_);
    cycle_waiter_ = std::move(waiter);
}

bool StrategyManager::wait_for_cycle(Strategy& strategy, double seconds) {
    CycleWaiter simulated;
    {
        std::lock_guard lock(mutex_);
        simulated = cycle_waiter_;
    }
    auto wait_stop = [&](double timeout) {
        if (simulated) return simulated(timeout) || strategy.stop_requested.load();
        std::unique_lock lock(strategy.wait_mutex);
        return strategy.wait_cv.wait_for(lock, std::chrono::duration<double>(std::max(0.0, timeout)),
                                         [&] { return strategy.stop_requested.load(); });
    };
    if (!reporting_) return wait_stop(seconds);
    double deadline = monotonic_() + seconds;
    while (!strategy.stop_requested.load()) {
        double remaining = deadline - monotonic_();
        if (remaining <= 0) return false;
        report("heartbeat_session", [&](reporting::SessionReporter& r) { r.heartbeat_session(strategy.session_id); });
        if (wait_stop(std::min(10.0, remaining))) return true;
    }
    return true;
}

void StrategyManager::run_strategy(std::shared_ptr<Strategy> strategy) {
    try {
        ValidatedValues v = validated_strategy_values(strategy->command);
        while (!strategy->stop_requested.load()) {
            report("heartbeat_session", [&](reporting::SessionReporter& r) { r.heartbeat_session(strategy->session_id); });
            double cycle_start = monotonic_();
            CycleResult result = run_cycle(strategy->command);
            bool test_mode = json_truthy(json_find(strategy->command, "fast_trading_test_mode"));
            long long published_count = test_mode ? 0 : static_cast<long long>(result.commands.size());
            std::string auto_stop_reason;
            {
                std::lock_guard lock(mutex_);
                strategy->cycles += 1;
                strategy->published_commands += published_count;
                strategy->last_cycle = result.to_json();
                if (!result.commands.empty()) strategy->consecutive_capacity_noops = 0;
                else if (result.capacity_noop) strategy->consecutive_capacity_noops += 1;
                // A cycle may finish after stop; retain the stop reason.
                if (!strategy->stop_requested.load()) {
                    if (result.target_position_reached) {
                        strategy->last_reason = kStopReasonTargetPosition;
                        auto_stop_reason = strategy->last_reason;
                    } else if (strategy->consecutive_capacity_noops >= kMaxConsecutiveCapacityNoops) {
                        strategy->last_reason = kStopReasonCapacityNoops;
                        auto_stop_reason = strategy->last_reason;
                    } else {
                        strategy->last_reason = result.reason;
                    }
                }
            }
            if (!auto_stop_reason.empty()) {
                NG_LOG_INFO(kLog, "Algo trading automatically stopped mode={} symbol={} key={} command_id={} reason={}",
                            json_to_string(json_find(strategy->command, "trading_mode")).value_or(""),
                            json_to_string(json_find(strategy->command, "symbol")).value_or(""), strategy->key,
                            strategy->session_id, auto_stop_reason);
                break;
            }
            double elapsed = monotonic_() - cycle_start;
            if (wait_for_cycle(*strategy, std::max(0.0, static_cast<double>(v.cycle_seconds) - elapsed))) break;
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
        auto it = strategies_.find(strategy->session_id);
        if (it != strategies_.end() && it->second == strategy) strategies_.erase(it);
        auto sit = stopping_strategies_.find(strategy->session_id);
        if (sit != stopping_strategies_.end() && sit->second == strategy) stopping_strategies_.erase(sit);
        reason = strategy->last_reason.empty() ? "Strategy stopped" : strategy->last_reason;
        cycles = strategy->cycles;
        published = strategy->published_commands;
        finished_.push_back(strategy);  // joined by the next manager call (never by the worker itself)
    }
    record_ended(*strategy, reason, cycles, published);
    report("end_session", [&](reporting::SessionReporter& r) {
        r.end_session(strategy->session_id, reason == "Strategy error" ? "failed" : "stopped", reason);
    });
    {
        std::lock_guard lock(strategy->wait_mutex);
        strategy->finished = true;
    }
    strategy->wait_cv.notify_all();
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
        // Build and reserve accounts atomically so simultaneous E/F or multi-symbol sessions cannot
        // both use the same account while a command is still being handed to the publisher.
        std::lock_guard lock(mutex_);
        std::map<std::string, double> effective = last_action_monotonic_by_account_;
        for (const auto& id : inflight_account_ids_) effective[id] = now;
        CycleInputs inputs;
        inputs.trading_mode = v.mode;
        inputs.symbol = v.symbol;
        inputs.book = book;
        inputs.price_limit = v.price_limit;
        if (settings_.price_tick) inputs.tick_size = settings_.price_tick(v.symbol,v.mode=="E");
        inputs.account_ids = v.account_ids;
        inputs.book_levels = v.book_levels;
        inputs.target_percentage = v.target_percentage;
        inputs.total_target_position = v.total_target_position;
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
    bool has_session = json_python_truthy(json_find(command, "command_id"));
    std::string session_id = command_session_id(command);
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
                if (has_session) {
                    if (reporting_) reporting_->register_command(session_id, cmd);
                }
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
        // A failed publish is not an account action. Release it (and any later commands that were
        // never attempted) without a cooldown.
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
                "target_qty={} allocated_qty={} price={} commands={} capacity_noop={} total_position={} "
                "total_target_position={} reason={}",
                result.trading_mode, result.symbol, result.resting_side, v.book_levels, v.target_percentage,
                result.total_resting_qty, result.target_quantity, result.allocated_quantity,
                result.limit_price ? format_double(*result.limit_price) : "None", result.commands.size(), result.capacity_noop,
                result.total_position ? format_double(*result.total_position) : "None",
                result.total_target_position ? std::to_string(*result.total_target_position) : "None", result.reason);
    return result;
}

std::vector<StrategyStatus> StrategyManager::statuses() const {
    std::lock_guard lock(mutex_);
    std::vector<StrategyStatus> out;
    auto add = [&](const std::shared_ptr<Strategy>& s, bool stopping) {
        StrategyStatus st;
        st.key = s->key;
        st.session_id = s->session_id;
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
    std::sort(out.begin(), out.end(), [](const StrategyStatus& a, const StrategyStatus& b) { return a.started_at < b.started_at; });
    return out;
}

}  // namespace ng::fast
