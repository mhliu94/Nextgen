#include "algo/smart_selling/runtime.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "algo/smart_selling/commands.h"

namespace ng::smart {
namespace {
constexpr const char* kLog = "trading-ui.smart-selling";
}

SmartSellingRuntime::SmartSellingRuntime(const SmartSellingConfig& config, MetasProvider metas, SnapshotsProvider snapshots,
                                         PublishCommand publish, FastConflict fast_conflict, FastActive fast_active,
                                         std::shared_ptr<SmartSellingEngine> engine, Clock clock, Clock wall_clock)
    : config_(config),
      metas_(std::move(metas)),
      snapshots_(std::move(snapshots)),
      publish_(std::move(publish)),
      fast_conflict_(fast_conflict ? std::move(fast_conflict) : FastConflict([](const std::string&, const std::string&) { return false; })),
      fast_active_(fast_active ? std::move(fast_active) : FastActive([] { return false; })),
      clock_(clock ? std::move(clock) : Clock(&mono_now)),
      wall_clock_(wall_clock ? std::move(wall_clock) : Clock(&wall_now)) {
    bool file_backed = false;
    if (!engine) {
        std::filesystem::path path(config_.state_db);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::string lock_path = path.string() + ".lock";
        lock_fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
        if (lock_fd_ < 0) throw std::runtime_error("cannot open Smart selling lock file " + lock_path);
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(lock_fd_);
            lock_fd_ = -1;
            throw std::runtime_error("another control server already owns the Smart selling journal " + path.string());
        }
        try {
            engine = std::make_shared<SmartSellingEngine>(path.string(), clock_, wall_clock_);
        } catch (...) {
            ::close(lock_fd_);
            lock_fd_ = -1;
            throw;
        }
        file_backed = true;
        owns_engine_ = true;
    }
    engine_ = std::move(engine);
    feedback_ = std::make_unique<SmartSellingFeedback>(*engine_, 10000, file_backed ? config_.state_db + ".feedback.sqlite3" : ":memory:");
    market_ = std::make_unique<SmartSellingMarketAdapter>(*engine_, clock_, wall_clock_, config_.trade_max_age);
    worker_ = std::thread([this] { worker_loop(); });
}

SmartSellingRuntime::~SmartSellingRuntime() { close(); }

void SmartSellingRuntime::start() {
    if (heartbeat_.joinable()) return;
    heartbeat_ = std::thread([this] { heartbeat_loop(); });
}

void SmartSellingRuntime::heartbeat_loop() {
    while (!stop_.load()) {
        double started = clock_();
        try {
            tick();
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "Smart selling controller heartbeat failed: {}", e.what());
            on_feed_state("orders", "disconnected");
        }
        double wait = std::max(0.01, 1.0 - (clock_() - started));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(wait);
        while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void SmartSellingRuntime::worker_loop() {
    while (true) {
        json action;
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [&] { return stop_.load() || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_.load()) return;
                continue;
            }
            action = std::move(queue_.front());
            queue_.pop_front();
            ++in_flight_;
        }
        try {
            publish_action(action);
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "Smart selling publisher worker failed: {}", e.what());
        }
        {
            std::lock_guard lock(queue_mutex_);
            --in_flight_;
        }
        queue_cv_.notify_all();
    }
}

void SmartSellingRuntime::wait_idle() {
    std::unique_lock lock(queue_mutex_);
    queue_cv_.wait_for(lock, std::chrono::seconds(5), [&] { return queue_.empty() && in_flight_ == 0; });
}

Decimal SmartSellingRuntime::available_position(const std::string& account_id, const std::string& symbol) {
    auto snapshots = snapshots_();
    auto it = snapshots.find(account_id);
    if (it == snapshots.end() || !it->second.ts || it->second.ts->empty()) {
        throw ValueError("A recent account position snapshot is required");
    }
    const AccountSnapshot& snapshot = it->second;
    auto parsed = parse_iso8601(*snapshot.ts);
    if (!parsed) throw ValueError("Account position snapshot is stale");
    double age = wall_clock_() - parsed->epoch_seconds;
    if (!parsed->has_offset || !(-1 <= age && age <= config_.account_max_age)) throw ValueError("Account position snapshot is stale");
    if (!snapshot.trading_enabled) throw ValueError("Trading is disabled for this account");
    Decimal quantity;
    for (const auto& position : snapshot.positions) {
        if (canonical_symbol(position.symbol) != symbol) continue;
        if (!std::isfinite(position.qty) || (position.available_qty && !std::isfinite(*position.available_qty))) {
            throw ValueError("Invalid account position quantity");
        }
        Decimal held = Decimal::from_double(position.qty);
        Decimal available = position.available_qty ? Decimal::from_double(*position.available_qty) : held;
        quantity += Decimal::min(held, available);
    }
    return Decimal::max(Decimal::zero(), quantity);
}

json SmartSellingRuntime::start_parent(const json& settings_in) {
    if (!settings_in.is_object()) throw ValueError("Smart selling settings must be an object");
    json settings = settings_in;
    json accounts_raw;
    if (settings.contains("account_ids") && !settings.at("account_ids").is_null()) accounts_raw = settings.at("account_ids");
    else accounts_raw = json::array({settings.value("account_id", json(""))});
    if (!accounts_raw.is_array() || accounts_raw.empty()) throw ValueError("Select at least one account");
    std::vector<std::string> accounts;
    for (const auto& a : accounts_raw) {
        if (!a.is_string() || trim(a.get<std::string>()).empty()) throw ValueError("Invalid account selection");
        accounts.push_back(trim(a.get<std::string>()));
    }
    {
        std::set<std::string> unique(accounts.begin(), accounts.end());
        if (unique.size() != accounts.size()) throw ValueError("Select each account only once");
    }
    const AccountDirectory& metas = metas_();
    for (const auto& a : accounts) if (!metas.contains(a)) throw ValueError("Unknown account");
    std::string symbol = canonical_symbol(json_to_string(json_find(settings, "symbol")).value_or(""));
    if (settings.contains("symbol") && settings.at("symbol").is_null()) symbol = "";
    if (symbol.empty()) throw ValueError("A symbol is required");
    if (settings.contains("imminent_event") && !settings.at("imminent_event").is_boolean()) {
        throw ValueError("imminent_event must be boolean");
    }
    std::lock_guard lock(mutex_);
    if (has_active_session()) throw ValueError("Smart selling already owns an active session; stop it before starting another");
    bool conflict = fast_active_();
    for (const auto& a : accounts) if (fast_conflict_(a, symbol)) conflict = true;
    if (conflict) throw ValueError("Stop the existing algorithm before starting another session");
    if (feeds_.at("orders") != "live") {
        throw ValueError("Wait for the order-status feed to finish reconciliation before starting Smart selling");
    }
    std::map<std::string, std::string> rules = config_.rules_for(symbol);
    Decimal quantity_step = Decimal::parse(rules.at("quantity_step"));
    std::map<std::string, Decimal> quantities;
    for (const auto& a : accounts) {
        if (feedback_->has_open_orders(a, symbol)) {
            throw ValueError("Existing orders for this account and instrument must reach terminal status first");
        }
        quantities[a] = available_position(a, symbol).round_down_to_step(quantity_step);
    }
    Decimal available;
    for (auto& [a, q] : quantities) available += q;
    Decimal quantity;
    const json* requested = json_find(settings, "quantity");
    if (!requested || requested->is_null()) {
        quantity = available;
    } else {
        auto parsed = Decimal::try_parse(json_to_string(requested).value_or(""));
        if (!parsed || requested->is_boolean()) throw ValueError("Quantity must be positive and finite");
        quantity = *parsed;
    }
    if (quantity <= Decimal::zero()) throw ValueError("Quantity must be positive and finite; no available position to sell");
    if (quantity > available) throw ValueError("Quantity exceeds the selected accounts' available position");
    json permitted = json::object();
    for (const char* key : {"mode", "start_time", "deadline", "minimum_price", "imminent_event"}) {
        if (settings.contains(key)) permitted[key] = settings.at(key);
    }
    std::string mode = to_lower(json_to_string(json_find(settings, "mode")).value_or(Defaults::mode));
    if (mode == "1") mode = "patient";
    else if (mode == "2") mode = "normal";
    else if (mode == "3") mode = "urgent";
    auto pct = config_.participation_percentages.find(mode);
    permitted["participation_percentage"] = pct == config_.participation_percentages.end() ? json(nullptr) : json(pct->second);
    permitted["account_id"] = accounts.front();
    permitted["account_ids"] = accounts;
    json account_quantities = json::object();
    for (const auto& a : accounts) account_quantities[a] = quantities.at(a).str();
    permitted["account_quantities"] = account_quantities;
    permitted["quantity"] = quantity.str();
    permitted["symbol"] = symbol;
    json instrument_rules = json::object();
    for (auto& [k, v] : rules) instrument_rules[k] = v;
    permitted["instrument_rules"] = instrument_rules;
    permitted["quote_max_age"] = config_.quote_max_age;
    permitted["trade_max_age"] = config_.trade_max_age;
    return engine_->start_parent(permitted);
}

std::vector<std::string> SmartSellingRuntime::parent_accounts(const json& parent) {
    const json* ids = json_find(parent, "account_ids");
    if (ids && ids->is_array() && !ids->empty()) return ids->get<std::vector<std::string>>();
    return {parent.at("account_id").get<std::string>()};
}

bool SmartSellingRuntime::has_active_session() { return !engine_->list_parents(true, 1).empty(); }

bool SmartSellingRuntime::owns_account(const std::string& account_id) {
    for (const json& parent : engine_->list_parents(true, 1000)) {
        auto accounts = parent_accounts(parent);
        if (std::find(accounts.begin(), accounts.end(), account_id) != accounts.end()) return true;
    }
    return false;
}

bool SmartSellingRuntime::owns(const std::string& account_id, const std::string& symbol) {
    std::string canonical = canonical_symbol(symbol);
    for (const json& parent : engine_->list_parents(true, 1000)) {
        auto accounts = parent_accounts(parent);
        if (std::find(accounts.begin(), accounts.end(), account_id) != accounts.end() &&
            parent.at("symbol").get<std::string>() == canonical) {
            return true;
        }
    }
    return false;
}

std::vector<json> SmartSellingRuntime::list_parents() {
    std::vector<json> parents = engine_->list_parents();
    for (json& parent : parents) {
        MarketStatus status = market_->market_status(parent.at("symbol").get<std::string>());
        parent["market_state"] = status.state;
        parent["market_reason"] = status.reason;
    }
    return parents;
}

json SmartSellingRuntime::stop_parent(const std::string& parent_id) {
    std::lock_guard lock(mutex_);
    json result = engine_->request_stop(parent_id);
    dispatch(engine_->drain_actions());
    return result;
}

int SmartSellingRuntime::stop_all() {
    std::lock_guard lock(mutex_);
    std::vector<json> parents = engine_->list_parents(true, 1000);
    for (const json& parent : parents) stop_parent(parent.at("parent_id").get<std::string>());
    return static_cast<int>(parents.size());
}

void SmartSellingRuntime::on_price_book(const json& payload, const std::optional<std::string>& topic, std::optional<int> partition,
                                        std::optional<long long> offset) {
    std::lock_guard lock(mutex_);
    market_->apply_price_book(payload, topic, partition, offset);
}

void SmartSellingRuntime::on_price_book_health(bool healthy) {
    std::lock_guard lock(mutex_);
    market_->set_connected(healthy);
}

void SmartSellingRuntime::on_volume_payload(const json& payload, const std::optional<std::string>& topic,
                                            std::optional<int> partition, std::optional<long long> offset) {
    std::lock_guard lock(mutex_);
    market_->apply_volume(payload, topic, partition, offset);
}

void SmartSellingRuntime::on_order_payload(const std::string& raw) {
    std::lock_guard lock(mutex_);
    feedback_->apply_snapshot(raw);
    dispatch(engine_->drain_actions());
}

void SmartSellingRuntime::track_external_order(const json& command) {
    std::lock_guard lock(mutex_);
    feedback_->track_external_order(command);
}

void SmartSellingRuntime::on_feed_state(const std::string& feed, const std::string& state) {
    std::lock_guard lock(mutex_);
    feeds_[feed] = state;
    if (feed == "volume") market_->set_volume_connected(state == "live");
    if (state != "live") {
        for (const json& parent : engine_->list_parents(true, 1000)) {
            engine_->set_feed_health(parent.at("symbol").get<std::string>(), false, feed + " feed " + state);
        }
    }
}

std::map<std::string, std::string> SmartSellingRuntime::feed_states() {
    std::lock_guard lock(mutex_);
    return feeds_;
}

void SmartSellingRuntime::set_feed_states_for_test(const std::map<std::string, std::string>& states) {
    std::lock_guard lock(mutex_);
    for (auto& [k, v] : states) feeds_[k] = v;
}

void SmartSellingRuntime::tick() {
    std::lock_guard lock(mutex_);
    std::vector<json> parents = engine_->list_parents(true, 1000);
    bool feeds_live = true;
    for (auto& [feed, state] : feeds_) if (state != "live") feeds_live = false;
    for (const json& parent : parents) {
        std::string symbol = parent.at("symbol").get<std::string>();
        if (!feeds_live) engine_->set_feed_health(symbol, false, "waiting for live order and volume feeds");
        try {
            for (const auto& account : parent_accounts(parent)) {
                const json* balance = json_find(parent.at("accounts"), account);
                if (balance && balance->is_object() && Decimal::parse(balance->at("remaining").get<std::string>()) <= Decimal::zero()) {
                    continue;
                }
                available_position(account, symbol);
            }
        } catch (const std::invalid_argument&) {
            engine_->set_feed_health(symbol, false, "account snapshot unavailable or trading disabled");
        } catch (const DecimalError&) {
            engine_->set_feed_health(symbol, false, "account snapshot unavailable or trading disabled");
        }
    }
    if (clock_() - last_forecast_ >= 30) {
        for (const json& parent : parents) {
            if (parent.at("deadline").is_null()) continue;
            auto rate = market_->volume_rate(parent.at("symbol").get<std::string>());
            if (!rate) continue;
            auto deadline = parse_iso8601(parent.at("deadline").get<std::string>());
            if (!deadline) continue;
            Decimal remaining = Decimal::from_double(std::max(0.0, deadline->epoch_seconds - wall_clock_()));
            engine_->set_remaining_volume_forecast(parent.at("parent_id").get<std::string>(), *rate * remaining);
        }
        last_forecast_ = clock_();
    }
    dispatch(engine_->tick());
}

void SmartSellingRuntime::dispatch(const std::vector<json>& actions) {
    for (const json& action : actions) {
        std::string type = action.at("type").get<std::string>();
        if (type == "query") {
            feedback_->reconcile(action.at("client_order_id").get<std::string>());
            continue;
        }
        {
            std::lock_guard lock(queue_mutex_);
            queue_.push_back(action);
        }
        queue_cv_.notify_all();
    }
}

std::optional<json> SmartSellingRuntime::prepare_submit(const json& action) {
    std::string client_id = action.at("client_order_id").get<std::string>();
    bool feeds_live = true;
    for (auto& [feed, state] : feeds_) if (state != "live") feeds_live = false;
    if (!feeds_live) {
        engine_->abort_dispatch(client_id, "waiting for live order and volume feeds");
        return std::nullopt;
    }
    std::string account_id = action.at("account_id").get<std::string>();
    std::string symbol = action.at("symbol").get<std::string>();
    Decimal available = available_position(account_id, symbol);
    if (Decimal::parse(action.at("quantity").get<std::string>()) > available) {
        engine_->abort_dispatch(client_id, "available position decreased", true);
        return std::nullopt;
    }
    if (feedback_->has_open_orders(account_id, symbol)) {
        engine_->abort_dispatch(client_id, "conflicting open order", true);
        return std::nullopt;
    }
    return engine_->prepare_dispatch(client_id);
}

void SmartSellingRuntime::publish_action(const json& action) {
    std::lock_guard lock(mutex_);
    handoff_action(action);
}

void SmartSellingRuntime::handoff_action(json action) {
    bool publish_started = false;
    std::string type = action.at("type").get<std::string>();
    try {
        if (type == "submit") {
            auto prepared = prepare_submit(action);
            if (!prepared) return;
            action = *prepared;
        } else if (type == "cancel") {
            auto context = engine_->get_order_context(action.at("client_order_id").get<std::string>());
            if (!context || !context->at("active").get<bool>()) return;
            for (const char* key : {"parent_id", "account_id", "symbol", "broker_order_id"}) {
                if (context->at(key) != action.at(key)) return;
            }
        }
        json command = build_smart_selling_command(action, metas_());
        publish_started = true;
        publish_(command, action.at("account_id").get<std::string>());
    } catch (const std::exception& e) {
        if (type == "submit" && !publish_started) {
            NG_LOG_WARN(kLog, "Smart selling child rejected before publication: {}", e.what());
            engine_->abort_dispatch(action.at("client_order_id").get<std::string>(), e.what(), true);
        } else {
            NG_LOG_ERROR(kLog, "Smart selling {} dispatch uncertain: {}", type, e.what());
            engine_->on_action_error(action.at("client_order_id").get<std::string>(), type, e.what());
        }
    }
}

void SmartSellingRuntime::close() {
    {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        closed_ = true;
    }
    try {
        stop_all();
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Smart selling stop_all during close failed: {}", e.what());
    }
    stop_.store(true);
    queue_cv_.notify_all();
    if (heartbeat_.joinable()) heartbeat_.join();
    if (worker_.joinable()) worker_.join();
    feedback_->close();
    if (owns_engine_) engine_->close();
    if (lock_fd_ >= 0) {
        ::close(lock_fd_);
        lock_fd_ = -1;
    }
}

}  // namespace ng::smart
