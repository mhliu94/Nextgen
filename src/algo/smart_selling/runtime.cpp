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
#include "orders/commands.h"

namespace ng::smart {
namespace {
constexpr const char* kLog = "trading-ui.smart-selling";
}

SmartSellingRuntime::SmartSellingRuntime(const SmartSellingConfig& config, MetasProvider metas, SnapshotsProvider snapshots,
                                         PublishCommand publish, OwnsAccount fast_owns_account,
                                         std::shared_ptr<SmartSellingEngine> engine, Clock clock, Clock wall_clock,
                                         reporting::SessionReporter* reporting)
    : SmartSellingRuntime(config, std::move(metas), std::move(snapshots), std::move(publish), std::move(fast_owns_account),
                          std::move(engine), std::move(clock), std::move(wall_clock), reporting, "G",
                          [](const std::string& path, const Clock& c, const Clock& w) {
                              return std::make_shared<SmartSellingEngine>(path, c, w);
                          }) {}

SmartSellingRuntime::SmartSellingRuntime(const SmartSellingConfig& config, MetasProvider metas, SnapshotsProvider snapshots,
                                         PublishCommand publish, OwnsAccount fast_owns_account,
                                         std::shared_ptr<SmartSellingEngine> engine, Clock clock, Clock wall_clock,
                                         reporting::SessionReporter* reporting, std::string trading_mode,
                                         EngineFactory engine_factory)
    : config_(config),
      metas_(std::move(metas)),
      snapshots_(std::move(snapshots)),
      publish_(std::move(publish)),
      // Fast trading (modes E/F) and the other Smart controller own accounts too; no account may be in two sessions.
      fast_owns_account_(fast_owns_account ? std::move(fast_owns_account) : OwnsAccount([](const std::string&) { return false; })),
      clock_(clock ? std::move(clock) : Clock(&mono_now)),
      wall_clock_(wall_clock ? std::move(wall_clock) : Clock(&wall_now)),
      reporting_(reporting),
      trading_mode_(std::move(trading_mode)) {
    bool file_backed = false;
    if (!engine) {
        std::filesystem::path path(config_.state_db);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::string lock_path = path.string() + ".lock";
        lock_fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
        if (lock_fd_ < 0) throw std::runtime_error("cannot open the Smart trading lock file " + lock_path);
        if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            ::close(lock_fd_);
            lock_fd_ = -1;
            throw std::runtime_error("another control server already owns the Smart trading journal " + path.string());
        }
        try {
            engine = engine_factory(path.string(), clock_, wall_clock_);
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
    sync_reporting();
}

SmartSellingRuntime::~SmartSellingRuntime() { close(); }

bool SmartSellingRuntime::report(const char* method, const std::function<void(reporting::SessionReporter&)>& call) {
    if (!reporting_) return false;
    try {
        call(*reporting_);
        return true;
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed updating Smart trading reporting method={}: {}", method, e.what());
        return false;
    }
}

bool SmartSellingRuntime::feeds_live() const {
    for (auto& [feed, state] : feeds_) {
        if (state != "live") return false;
    }
    return true;
}

void SmartSellingRuntime::sync_reporting(const std::vector<json>* parents_in, const json* parameters) {
    if (!reporting_) return;
    try {
        std::vector<json> loaded;
        if (!parents_in) loaded = engine_->list_parents(false, 1000);
        const std::vector<json>& parents = parents_in ? *parents_in : loaded;
        for (const json& parent : parents) {
            std::string session_id = parent.at("parent_id").get<std::string>();
            if (!reported_sessions_.count(session_id)) {
                // Existing durable rows retain their original parameters on recovery.
                json starting = json::object();
                for (const char* key : {"symbol", "quantity", "mode", "participation", "start", "deadline", "minimum_price",
                                        "maximum_price", "child_cap", "disable_child_cap"}) {
                    starting[key] = parent.value(key, json(nullptr));
                }
                std::vector<std::string> accounts = parent_accounts(parent);
                starting["account_ids"] = accounts;
                json quantities = json::object();
                if (const json* balances = json_find(parent, "accounts"); balances && balances->is_object()) {
                    for (auto it = balances->begin(); it != balances->end(); ++it) quantities[it.key()] = it.value().at("quantity");
                }
                starting["account_quantities"] = quantities;
                starting["trading_mode"] = trading_mode_;
                if (parameters && parameters->is_object()) starting.update(*parameters);
                std::optional<std::string> started_at;
                if (const json* start = json_find(parent, "start"); start && start->is_string() && !start->get<std::string>().empty()) {
                    started_at = start->get<std::string>();
                }
                if (report("start_session",
                           [&](reporting::SessionReporter& r) { r.start_session(session_id, starting, accounts, started_at); })) {
                    reported_sessions_.insert(session_id);
                }
                // A recovered outstanding child can still receive late fills.
                if (const json* order = json_find(parent, "active_order"); order && order->is_object()) {
                    json command;
                    command["account_id"] = order->value("account_id", parent.at("account_id"));
                    command["symbol"] = parent.at("symbol");
                    command["command_id"] = order->at("client_order_id");
                    command["client_order_id"] = order->at("client_order_id");
                    report("register_command", [&](reporting::SessionReporter& r) { r.register_command(session_id, command); });
                }
            }
            std::string state = to_lower(parent.at("state").get<std::string>());
            if (parent.at("active").get<bool>()) {
                if (state != "stopping" && parent.value("order_state", json(nullptr)) == json("RECONCILE")) {
                    state = "reconciling";
                } else if (state == "hold" || (state == "running" && !feeds_live())) {
                    state = "paused";
                }
                report("heartbeat_session", [&](reporting::SessionReporter& r) { r.heartbeat_session(session_id, state); });
            } else if (!reported_terminal_.count(session_id)) {
                std::optional<std::string> reason;
                if (const json* text = json_find(parent, "reason"); text && text->is_string()) reason = text->get<std::string>();
                if (report("end_session", [&](reporting::SessionReporter& r) { r.end_session(session_id, state, reason); })) {
                    reported_terminal_.insert(session_id);
                }
            }
        }
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed reading Smart trading sessions for reporting: {}", e.what());
    }
}

std::optional<Decimal> SmartSellingRuntime::account_average_cost(const std::string& account_id, const std::string& symbol) {
    auto snapshots = snapshots_();
    auto it = snapshots.find(account_id);
    if (it == snapshots.end()) return std::nullopt;
    // Arithmetic follows Python's default Decimal context (28 significant digits, half-even), so the
    // persisted cost strings are identical to the original controller's.
    Decimal total, cost;
    try {
        for (const auto& position : it->second.positions) {
            if (canonical_symbol(position.symbol) != symbol) continue;
            if (!std::isfinite(position.qty)) return std::nullopt;
            Decimal quantity = Decimal::from_python_float(position.qty);
            if (quantity <= Decimal::zero()) continue;
            if (!position.avg_price || !std::isfinite(*position.avg_price)) return std::nullopt;
            Decimal average = Decimal::from_python_float(*position.avg_price);
            if (average.is_negative()) return std::nullopt;
            total = (total + quantity).round_significant(28);
            cost = (cost + (quantity * average).round_significant(28)).round_significant(28);
        }
        if (total.is_zero()) return std::nullopt;
        return Decimal::python_div(cost, total, 28);
    } catch (const DecimalError&) {
        return std::nullopt;
    }
}

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
    // Each account may participate in at most one algorithm session (E, F, G or H). Stopping and
    // reconciling sessions keep their accounts reserved.
    std::vector<std::string> owned;
    for (const auto& a : accounts) {
        if (owns_account(a) || fast_owns_account_(a)) owned.push_back(a);
    }
    if (!owned.empty()) throw ValueError(orders::algo_account_conflict_error(owned));
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
    for (const char* key : {"mode", "start_time", "deadline", "minimum_price", "imminent_event", "disable_child_cap"}) {
        if (settings.contains(key)) permitted[key] = settings.at(key);
    }
    std::string mode = to_lower(json_to_string(json_find(settings, "mode")).value_or(Defaults::mode));
    if (mode == "1") mode = "patient";
    else if (mode == "2") mode = "normal";
    else if (mode == "3") mode = "urgent";
    else if (mode == "4") mode = "ultra";
    // Trusted application settings only; HTTP rate overrides are ignored.
    auto pct = config_.participation_percentages.find(mode);
    permitted["participation_percentage"] = pct == config_.participation_percentages.end() ? json(nullptr) : json(pct->second);
    permitted["account_id"] = accounts.front();
    permitted["account_ids"] = accounts;
    json average_costs = json::object();
    for (const auto& a : accounts) {
        auto cost = account_average_cost(a, symbol);
        average_costs[a] = cost ? json(cost->str()) : json(nullptr);
    }
    permitted["account_average_costs"] = average_costs;
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
    json parent = engine_->start_parent(permitted);
    std::vector<json> started{parent};
    sync_reporting(&started, &permitted);
    return parent;
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
    // Older active sessions must remain visible beyond the recent-history limit.
    std::set<std::string> listed;
    for (const json& parent : parents) listed.insert(parent.at("parent_id").get<std::string>());
    std::vector<json> active = engine_->list_parents(true, 1000);
    for (json& parent : active) {
        std::string id = parent.at("parent_id").get<std::string>();
        if (listed.insert(id).second) {
            parents.push_back(std::move(parent));
        } else {
            for (json& existing : parents) {
                if (existing.at("parent_id").get<std::string>() == id) existing = parent;
            }
        }
    }
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
    std::vector<json> stopped{engine_->get_parent(parent_id)};
    sync_reporting(&stopped);
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
    sync_reporting();
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
    sync_reporting();
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
    bool live = feeds_live();
    for (const json& parent : parents) {
        std::string symbol = parent.at("symbol").get<std::string>();
        if (!live) engine_->set_feed_health(symbol, false, "waiting for live order and volume feeds");
        try {
            for (const auto& account : parent_accounts(parent)) {
                const json* balance = json_find(parent.at("accounts"), account);
                if (balance && balance->is_object() && Decimal::parse(balance->at("remaining").get<std::string>()) <= Decimal::zero()) {
                    continue;
                }
                available_position(account, symbol);
            }
            if (trading_mode_ == "G") {
                std::map<std::string, std::optional<Decimal>> costs;
                for (const auto& account : parent_accounts(parent)) costs[account] = account_average_cost(account, symbol);
                engine_->set_account_costs(parent.at("parent_id").get<std::string>(), costs);
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
    sync_reporting();
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
    if (!feeds_live()) {
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
    // The injected publisher must enqueue locally, without awaiting delivery.
    bool publish_started = false;
    std::string type = action.at("type").get<std::string>();
    struct SyncOnExit {
        SmartSellingRuntime& self;
        ~SyncOnExit() { self.sync_reporting(); }
    } sync_on_exit{*this};
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
        if (type == "submit") {
            std::string session_id = action.at("parent_id").get<std::string>();
            report("register_command", [&](reporting::SessionReporter& r) { r.register_command(session_id, command); });
        }
        publish_started = true;
        publish_(command, action.at("account_id").get<std::string>());
    } catch (const std::exception& e) {
        if (type == "submit" && !publish_started) {
            NG_LOG_WARN(kLog, "Smart trading child rejected before publication: {}", e.what());
            engine_->abort_dispatch(action.at("client_order_id").get<std::string>(), e.what(), true);
        } else {
            NG_LOG_ERROR(kLog, "Smart trading {} dispatch uncertain: {}", type, e.what());
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
