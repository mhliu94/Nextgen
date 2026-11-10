#include "algo/smart_selling/engine.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"

namespace ng::smart {
namespace {

constexpr const char* kLog = "trading-ui.smart-selling.engine";

Decimal D(const json& value) { return parse_decimal(value); }
Decimal D(const json& parent, const char* key) { return parse_decimal(parent.at(key)); }
std::string S(const Decimal& value) { return decimal_string(value); }

std::optional<double> opt_double(const json& value) {
    if (value.is_null()) return std::nullopt;
    return value.get<double>();
}

json opt_json(const std::optional<double>& value) { return value ? json(*value) : json(nullptr); }

std::optional<long long> opt_int(const json& value) {
    if (value.is_null()) return std::nullopt;
    return value.get<long long>();
}

std::string uuid_hex() { return uuid4_hex(); }

bool is_blank(const json& value) {
    return value.is_null() || (value.is_string() && value.get<std::string>().empty());
}

}  // namespace

bool is_terminal_status(const std::string& status) {
    const auto& v = terminal_order_statuses();
    return std::find(v.begin(), v.end(), status) != v.end();
}

bool is_working_status(const std::string& status) {
    const auto& v = working_order_statuses();
    return std::find(v.begin(), v.end(), status) != v.end();
}

template <typename Fn>
auto SmartSellingEngine::transaction(Fn&& fn) -> decltype(fn()) {
    std::lock_guard lock(mutex_);
    if (!db_) throw std::runtime_error("Smart selling engine is closed");
    db_->begin_immediate();
    try {
        if constexpr (std::is_void_v<decltype(fn())>) {
            fn();
            db_->commit();
        } else {
            auto result = fn();
            db_->commit();
            return result;
        }
    } catch (...) {
        db_->rollback();
        throw;
    }
}

SmartSellingEngine::SmartSellingEngine(const std::string& db_path, Clock clock, Clock wall_clock, int max_active_parents)
    : SmartSellingEngine(db_path, std::move(clock), std::move(wall_clock), max_active_parents, DeferRecovery{}) {
    recover();
}

SmartSellingEngine::SmartSellingEngine(const std::string& db_path, Clock clock, Clock wall_clock, int max_active_parents,
                                       DeferRecovery)
    : clock_(clock ? std::move(clock) : Clock(&mono_now)),
      wall_clock_(wall_clock ? std::move(wall_clock) : Clock(&wall_now)),
      max_active_parents_(max_active_parents),
      db_(std::make_unique<Sqlite>(db_path)) {
    db_->exec("PRAGMA journal_mode=WAL");
    db_->exec("PRAGMA synchronous=FULL");
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS smart_parents (
            id TEXT PRIMARY KEY, account_id TEXT NOT NULL, symbol TEXT NOT NULL,
            active INTEGER NOT NULL, data TEXT NOT NULL);
        CREATE UNIQUE INDEX IF NOT EXISTS smart_active_slot
            ON smart_parents(account_id,symbol) WHERE active=1;
        CREATE TABLE IF NOT EXISTS smart_orders (
            id TEXT PRIMARY KEY, parent_id TEXT NOT NULL, data TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS smart_tape (
            parent_id TEXT NOT NULL, event_id TEXT NOT NULL, quantity TEXT NOT NULL,
            rate TEXT NOT NULL, net_quantity TEXT NOT NULL, original_id TEXT,
            PRIMARY KEY(parent_id,event_id));
        CREATE INDEX IF NOT EXISTS smart_tape_original ON smart_tape(event_id);
        CREATE TABLE IF NOT EXISTS smart_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT, parent_id TEXT, ts REAL NOT NULL,
            kind TEXT NOT NULL, data TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS smart_market_checkpoints (
            symbol TEXT PRIMARY KEY, data TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS smart_outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT, data TEXT NOT NULL,
            dispatched INTEGER NOT NULL DEFAULT 0);
    )SQL");
}

void SmartSellingEngine::recover() {
    transaction([&] {
        // A persisted intent may have reached the broker even if dispatch was interrupted.
        // Never replay a submit after process restart.
        db_->exec("UPDATE smart_outbox SET dispatched=1 WHERE dispatched=0");
        double now = clock_();
        double wall = wall_clock_();
        for (json parent : active()) {
            parent["start_mono"] = now + parent.at("start_wall").get<double>() - wall;
            if (parent.at("deadline_wall").is_null()) parent["deadline_mono"] = nullptr;
            else parent["deadline_mono"] = now + parent.at("deadline_wall").get<double>() - wall;
            if (!parent.at("last_send_wall").is_null()) {
                parent["last_send"] = now - std::max(0.0, wall - parent.at("last_send_wall").get<double>());
            }
            parent["last_diagnostic"] = nullptr;
            if (!parent.at("order").is_null()) {
                json& order = parent["order"];
                order["send_mono"] = now - std::max(0.0, wall - order.at("send_wall").get<double>());
                order["state"] = "RECONCILE";
                order["reconcile_cause"] = "restart";
                order["last_query"] = nullptr;
                order["cancel_intent"] = true;
                order["cancel_at"] = nullptr;
                parent["reason"] = "restart reconciliation required";
            }
            save(parent);
            event(parent, "restart", json{{"reserved", parent.at("R")}});
        }
    });
}

SmartSellingEngine::~SmartSellingEngine() { close(); }

void SmartSellingEngine::close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        db_->close();
        db_.reset();
    }
}

std::vector<json> SmartSellingEngine::active(const std::optional<std::string>& symbol) {
    std::vector<json> out;
    std::string sql = "SELECT data FROM smart_parents WHERE active=1";
    if (symbol) sql += " AND symbol=?";
    Statement stmt(*db_, sql);
    if (symbol) stmt.bind(1, *symbol);
    while (stmt.step()) out.push_back(upgrade_parent(json::parse(stmt.column_text(0))));
    return out;
}

json SmartSellingEngine::load(const std::string& parent_id) {
    Statement stmt(*db_, "SELECT data FROM smart_parents WHERE id=?");
    stmt.bind(1, parent_id);
    if (!stmt.step()) throw KeyError(parent_id);
    return upgrade_parent(json::parse(stmt.column_text(0)));
}

json SmartSellingEngine::upgrade_parent(json parent) {
    std::string account = parent.at("account_id").get<std::string>();
    if (!parent.contains("account_ids")) parent["account_ids"] = json::array({account});
    if (!parent.contains("accounts")) {
        parent["accounts"] = json{{account, json{{"quantity", parent.at("quantity")}, {"F", parent.at("F")}, {"R", parent.at("R")}}}};
    }
    if (!parent.contains("next_account_index")) parent["next_account_index"] = 0;
    if (!parent.contains("slice_remaining")) parent["slice_remaining"] = "0";
    if (!parent.contains("disable_child_cap")) parent["disable_child_cap"] = false;
    // Every child needs at least 100 shares (or the larger configured minimum) on the quantity grid.
    Decimal minimum = Decimal::max(min_order_quantity(), D(parent, "min_quantity")).round_up_to_step(D(parent, "quantity_step"));
    parent["min_quantity"] = S(minimum);
    if (!parent.contains("base_child_cap")) parent["base_child_cap"] = parent.at("child_cap");
    for (const char* key : {"child_cap", "base_child_cap"}) parent[key] = S(Decimal::max(minimum, D(parent, key)));
    if (!parent.at("order").is_null() && !parent["order"].contains("account_id")) parent["order"]["account_id"] = account;
    return parent;
}

void SmartSellingEngine::save(const json& parent) {
    Decimal earned = D(parent, "E"), credit = D(parent, "C"), reserved = D(parent, "R"), filled = D(parent, "F"),
            discarded = D(parent, "D");
    if (earned != credit + reserved + filled + discarded || reserved.is_negative() || filled.is_negative() ||
        filled + reserved > D(parent, "quantity")) {
        throw std::logic_error("Smart selling ledger invariant violated");
    }
    Decimal sum_f, sum_r;
    bool account_ok = true;
    for (auto it = parent.at("accounts").begin(); it != parent.at("accounts").end(); ++it) {
        Decimal f = D(it.value(), "F"), r = D(it.value(), "R"), q = D(it.value(), "quantity");
        sum_f += f;
        sum_r += r;
        if (f.is_negative() || r.is_negative() || f + r > q) account_ok = false;
    }
    if (sum_f != filled || sum_r != reserved || !account_ok) {
        throw std::logic_error("Smart selling account inventory invariant violated");
    }
    Statement stmt(*db_,
                   "INSERT INTO smart_parents(id,account_id,symbol,active,data) VALUES(?,?,?,?,?) "
                   "ON CONFLICT(id) DO UPDATE SET active=excluded.active,data=excluded.data");
    stmt.bind(1, parent.at("parent_id").get<std::string>());
    stmt.bind(2, parent.at("account_id").get<std::string>());
    stmt.bind(3, parent.at("symbol").get<std::string>());
    stmt.bind(4, static_cast<long long>(parent.at("active").get<bool>() ? 1 : 0));
    stmt.bind(5, json_dump_compact(parent));
    stmt.exec();
    if (!parent.at("order").is_null()) save_order(parent.at("parent_id").get<std::string>(), parent.at("order"));
}

void SmartSellingEngine::save_order(const std::string& parent_id, const json& order) {
    Statement stmt(*db_,
                   "INSERT INTO smart_orders(id,parent_id,data) VALUES(?,?,?) "
                   "ON CONFLICT(id) DO UPDATE SET data=excluded.data");
    stmt.bind(1, order.at("client_order_id").get<std::string>());
    stmt.bind(2, parent_id);
    stmt.bind(3, json_dump_compact(order));
    stmt.exec();
}

void SmartSellingEngine::event(const json& parent, const std::string& kind, json detail) {
    if (!detail.is_object()) detail = json::object();
    for (const char* key : {"E", "C", "R", "F", "D"}) detail[key] = parent.at(key);
    Statement stmt(*db_, "INSERT INTO smart_events(parent_id,ts,kind,data) VALUES(?,?,?,?)");
    stmt.bind(1, parent.at("parent_id").get<std::string>());
    stmt.bind(2, wall_clock_());
    stmt.bind(3, kind);
    stmt.bind(4, json_dump_compact(detail));
    stmt.exec();
}

void SmartSellingEngine::trim(json& parent) {
    Decimal credit_cap = parent.at("disable_child_cap").get<bool>() ? D(parent, "quantity") - D(parent, "F") - D(parent, "R")
                                                                    : Decimal(2) * D(parent, "child_cap");
    Decimal excess = Decimal::max(Decimal::zero(), D(parent, "C") - credit_cap);
    if (!excess.is_zero()) {
        parent["C"] = S(D(parent, "C") - excess);
        parent["D"] = S(D(parent, "D") + excess);
        event(parent, "credit_discarded", json{{"quantity", S(excess)}});
    }
}

json SmartSellingEngine::start_parent(const json& settings) {
    double now = clock_();
    double wall = wall_clock_();
    json selected;
    if (settings.contains("account_ids") && !settings.at("account_ids").is_null()) selected = settings.at("account_ids");
    else selected = json::array({settings.value("account_id", json(nullptr))});
    if (!selected.is_array()) throw ValueError("account_ids must be a list");
    std::vector<std::string> account_ids;
    for (const auto& v : selected) {
        std::string id = v.is_null() ? "" : ng::trim(json_to_string(&v).value_or(""));
        if (std::find(account_ids.begin(), account_ids.end(), id) == account_ids.end()) account_ids.push_back(id);
    }
    std::string symbol = to_upper(ng::trim(json_to_string(json_find(settings, "symbol")).value_or("")));
    bool all_nonempty = !account_ids.empty();
    for (auto& id : account_ids) if (id.empty()) all_nonempty = false;
    if (!all_nonempty || symbol.empty()) throw ValueError("account_id or account_ids and symbol are required");
    std::string account = account_ids.front();
    Decimal quantity = parse_input_decimal(settings.value("quantity", json(nullptr)), "quantity", true);
    std::string mode = to_lower(json_to_string(json_find(settings, "mode")).value_or(Defaults::mode));
    if (mode == "1") mode = "patient";
    else if (mode == "2") mode = "normal";
    else if (mode == "3") mode = "urgent";
    else if (mode == "4") mode = "ultra";
    if (!mode_percentages().count(mode)) throw ValueError("mode must be one of: " + join(mode_names(), ", "));
    json disable_child_cap = settings.value("disable_child_cap", json(false));
    if (!disable_child_cap.is_boolean()) throw ValueError("disable_child_cap must be a boolean");
    // An explicit participation_percentage (even null) replaces the mode's default.
    json percentage_raw = settings.contains("participation_percentage") ? settings.at("participation_percentage")
                                                                        : json(mode_percentages().at(mode).str());
    Decimal percentage = parse_participation_percentage(percentage_raw);
    Decimal participation = percentage.scaleb(-2);
    const json& rules = (settings.contains("instrument_rules") && settings.at("instrument_rules").is_object())
                            ? settings.at("instrument_rules")
                            : settings;
    Decimal quantity_step = parse_input_decimal(rules.value("quantity_step", json(nullptr)), "quantity_step", true);
    Decimal tick_size = parse_input_decimal(rules.value("tick_size", json(nullptr)), "tick_size", true);
    Decimal minimum_quantity = parse_input_decimal(rules.value("min_quantity", json(nullptr)), "min_quantity", true);
    minimum_quantity = Decimal::max(min_order_quantity(), minimum_quantity).round_up_to_step(quantity_step);
    Decimal min_notional = parse_input_decimal(rules.value("min_notional", json(0)), "min_notional", false, true);
    if (quantity != quantity.round_down_to_step(quantity_step) || quantity < minimum_quantity) {
        throw ValueError("quantity must satisfy the instrument minimum and quantity step");
    }
    json inventory = settings.value("account_quantities", json(nullptr));
    if (inventory.is_null()) {
        if (account_ids.size() != 1) throw ValueError("account_quantities are required for multiple accounts");
        inventory = json{{account, S(quantity)}};
    }
    if (!inventory.is_object()) throw ValueError("account_quantities must include every selected account");
    for (auto& id : account_ids) {
        if (!inventory.contains(id)) throw ValueError("account_quantities must include every selected account");
    }
    json accounts = json::object();
    Decimal inventory_total;
    for (auto& id : account_ids) {
        Decimal available = parse_input_decimal(inventory.at(id), "account quantity", false, true);
        if (available != available.round_down_to_step(quantity_step)) {
            throw ValueError("account quantities must satisfy the instrument quantity step");
        }
        accounts[id] = json{{"quantity", S(available)}, {"F", "0"}, {"R", "0"}};
        inventory_total += available;
    }
    if (quantity > inventory_total) throw ValueError("quantity exceeds the selected accounts' available inventory");
    json start_value = settings.contains("start") ? settings.at("start") : settings.value("start_time", json(nullptr));
    if (start_value.is_string() && start_value.get<std::string>() == "now") start_value = nullptr;
    double start = parse_timestamp(start_value).value_or(wall);
    double horizon = parse_input_decimal(settings.value("horizon_seconds", json(Defaults::horizon_seconds)), "horizon_seconds", true)
                         .to_double();
    bool no_deadline = (settings.contains("no_deadline") && settings.at("no_deadline").is_boolean() &&
                        settings.at("no_deadline").get<bool>()) ||
                       (settings.contains("deadline") && settings.at("deadline").is_null());
    std::optional<double> deadline;
    if (!no_deadline) deadline = parse_timestamp(settings.value("deadline", json(nullptr))).value_or(start + horizon);
    if (deadline && (*deadline <= start || *deadline <= wall)) {
        throw ValueError("deadline must be later than start and current time");
    }
    json minimum_price = nullptr;
    if (settings.contains("minimum_price") && !is_blank(settings.at("minimum_price"))) {
        minimum_price = S(parse_input_decimal(settings.at("minimum_price"), "minimum_price", true));
    }
    Decimal denominator = parse_input_decimal(json(deadline ? *deadline - start : horizon));
    Decimal child_cap = Decimal::max(minimum_quantity,
                                     (quantity * Decimal(30)).div(denominator, 30).round_down_to_step(quantity_step));
    double quote_max_age =
        parse_input_decimal(settings.value("quote_max_age", json(Defaults::quote_max_age)), "quote_max_age", true).to_double();
    double trade_max_age =
        parse_input_decimal(settings.value("trade_max_age", json(Defaults::trade_max_age)), "trade_max_age", true).to_double();
    std::string tif = to_upper(json_to_string(json_find(settings, "time_in_force")).value_or(Defaults::time_in_force));
    json parent;
    std::string parent_id = json_to_string(json_find(settings, "parent_id")).value_or("");
    if (parent_id.empty() || json_find(settings, "parent_id") == nullptr || settings.at("parent_id").is_null()) parent_id = uuid_hex();
    parent["parent_id"] = parent_id;
    parent["account_id"] = account;
    parent["account_ids"] = account_ids;
    parent["accounts"] = accounts;
    parent["next_account_index"] = 0;
    parent["slice_remaining"] = "0";
    parent["symbol"] = symbol;
    parent["quantity"] = S(quantity);
    parent["mode"] = mode;
    parent["participation"] = S(participation);
    parent["minimum_price"] = minimum_price;
    parent["child_cap"] = S(child_cap);
    parent["base_child_cap"] = S(child_cap);
    parent["disable_child_cap"] = disable_child_cap.get<bool>();
    parent["tick_size"] = S(tick_size);
    parent["quantity_step"] = S(quantity_step);
    parent["min_quantity"] = S(minimum_quantity);
    parent["min_notional"] = S(min_notional);
    parent["time_in_force"] = tif;
    parent["start_wall"] = start;
    parent["deadline_wall"] = opt_json(deadline);
    parent["start_mono"] = now + start - wall;
    parent["deadline_mono"] = deadline ? json(now + *deadline - wall) : json(nullptr);
    parent["last_send"] = nullptr;
    parent["last_send_wall"] = nullptr;
    parent["E"] = "0";
    parent["C"] = "0";
    parent["R"] = "0";
    parent["F"] = "0";
    parent["D"] = "0";
    parent["order"] = nullptr;
    parent["fallback"] = false;
    parent["imminent_event"] = settings.contains("imminent_event") && json_python_truthy(&settings.at("imminent_event"));
    parent["active"] = true;
    parent["stop_requested"] = false;
    parent["stop_reason"] = "";
    parent["hold"] = "";
    parent["state"] = start > wall ? "SCHEDULED" : "RUNNING";
    parent["reason"] = "waiting for eligible tape volume";
    parent["quote_max_age"] = quote_max_age;
    parent["trade_max_age"] = trade_max_age;
    parent["trade_sequence"] = nullptr;
    parent["tape_gap"] = false;
    parent["remaining_volume_forecast"] = nullptr;
    parent["capacity"] = nullptr;
    parent["last_diagnostic"] = nullptr;
    parent["depth_cap_count"] = 0;
    parent["fills_after_cancel"] = "0";
    parent["last_tick"] = nullptr;
    parent["latencies"] = json::object();
    if (tif != "DAY" && tif != "GTC") throw ValueError("Smart selling requires ordinary DAY or GTC limits");
    configure_parent(parent, settings);
    register_parent(parent);
    return make_public(parent);
}

void SmartSellingEngine::configure_parent(json& parent, const json& settings) {
    json empty = json::object();
    const json* costs = json_find(settings, "account_average_costs");
    if (!costs) costs = &empty;
    for (auto it = parent["accounts"].begin(); it != parent["accounts"].end(); ++it) {
        const json* value = json_find(*costs, it.key());
        if (value && !value->is_null()) it.value()["average_cost"] = S(parse_decimal(*value, "average cost", false, true));
        else it.value()["average_cost"] = nullptr;
    }
}

void SmartSellingEngine::set_account_costs(const std::string& parent_id, const std::map<std::string, std::optional<Decimal>>& costs) {
    transaction([&] {
        json parent = load(parent_id);
        bool changed = false;
        for (auto it = parent["accounts"].begin(); it != parent["accounts"].end(); ++it) {
            json value = nullptr;
            auto found = costs.find(it.key());
            if (found != costs.end() && found->second) {
                if (found->second->is_negative()) throw ValueError("invalid average cost");
                value = S(*found->second);
            }
            if (it.value().value("average_cost", json(nullptr)) != value) {
                it.value()["average_cost"] = value;
                changed = true;
            }
        }
        if (changed) save(parent);
    });
}

std::optional<Decimal> SmartSellingEngine::depth_budget(const QuoteState&) const {
    // The 10% displayed-size rule is not enforced for Smart selling.
    return std::nullopt;
}

std::pair<Decimal, bool> SmartSellingEngine::dispatch_price(const json& parent, const json& order, const QuoteState& quote) const {
    Decimal floor = is_blank(parent.at("minimum_price")) ? Decimal::zero() : D(parent, "minimum_price");
    Decimal price = Decimal::max(Decimal::max(D(order, "limit_price"), quote.bid), floor).round_up_to_step(D(parent, "tick_size"));
    return {price, order.at("aggressive").get<bool>() || D(order, "limit_price") <= quote.bid};
}

Decimal SmartSellingEngine::account_capacity(const json& parent, const std::string& account_id, const Decimal&,
                                             bool include_order) const {
    const json& account = parent.at("accounts").at(account_id);
    Decimal available = D(account, "quantity") - D(account, "F") - D(account, "R");
    const json& order = parent.at("order");
    if (include_order && !order.is_null() && order.at("account_id").get<std::string>() == account_id) {
        available += D(order, "quantity");
    }
    return available;
}

std::vector<std::size_t> SmartSellingEngine::account_selection_indices(const json& parent) const {
    std::vector<std::string> accounts = parent.at("account_ids").get<std::vector<std::string>>();
    std::size_t start = static_cast<std::size_t>(parent.at("next_account_index").get<long long>());
    std::vector<std::size_t> indices;
    for (std::size_t offset = 0; offset < accounts.size(); ++offset) indices.push_back((start + offset) % accounts.size());
    if (std::string(side()) == "SELL") {
        // Sell first from the account whose average holding cost is closest to the live midpoint.
        // Distances are compared doubled (|2*cost - (bid + ask)|) to stay exact; the stable sort
        // rotates ties in distance and accounts with unknown costs.
        const QuoteState& quote = quotes_.at(parent.at("symbol").get<std::string>());
        Decimal doubled_mid = quote.bid + quote.ask;
        auto priority = [&](std::size_t index) -> std::pair<bool, Decimal> {
            const json& account = parent.at("accounts").at(accounts[index]);
            const json* cost = json_find(account, "average_cost");
            if (!cost || cost->is_null()) return {true, Decimal::zero()};
            return {false, (Decimal(2) * parse_decimal(*cost) - doubled_mid).abs()};
        };
        std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
            auto pa = priority(a), pb = priority(b);
            if (pa.first != pb.first) return !pa.first;
            return pa.second < pb.second;
        });
    }
    return indices;
}

void SmartSellingEngine::register_parent(const json& parent) {
    transaction([&] {
        if (static_cast<int>(active().size()) >= max_active_parents_) {
            throw ValueError("maximum active Smart selling parents reached");
        }
        {
            Statement exists(*db_, "SELECT 1 FROM smart_parents WHERE id=?");
            exists.bind(1, parent.at("parent_id").get<std::string>());
            if (exists.step()) throw ValueError("parent_id already exists");
        }
        std::vector<std::string> selected = parent.at("account_ids").get<std::vector<std::string>>();
        Statement same_symbol(*db_, "SELECT data FROM smart_parents WHERE symbol=?");
        same_symbol.bind(1, parent.at("symbol").get<std::string>());
        while (same_symbol.step()) {
            json existing = upgrade_parent(json::parse(same_symbol.column_text(0)));
            bool intersects = false;
            for (const auto& id : existing.at("account_ids")) {
                if (std::find(selected.begin(), selected.end(), id.get<std::string>()) != selected.end()) intersects = true;
            }
            if (!intersects) continue;
            if (existing.at("active").get<bool>()) {
                throw ValueError("an active Smart selling parent already owns this account/instrument");
            }
            if (!existing.at("hold").get<std::string>().empty()) {
                throw ValueError("unresolved Smart selling ledger conflict requires reconciliation");
            }
        }
        save(parent);
        event(parent, "parent_started", json{{"mode", parent.at("mode")}, {"participation", parent.at("participation")}});
    });
}

void SmartSellingEngine::on_quote(const std::string& symbol_in, const QuoteUpdate& update) {
    std::string symbol = to_upper(symbol_in);
    double now = update.received_at ? *update.received_at : clock_();
    transaction([&] {
        std::vector<json> parents = active(symbol);
        if (parents.empty()) return;
        auto old_it = quotes_.find(symbol);
        const QuoteState* old = old_it == quotes_.end() ? nullptr : &old_it->second;
        bool healthy = update.healthy;
        std::optional<long long> sequence_number = update.sequence;
        if (old && sequence_number && old->sequence && !update.reset_sequence) {
            if (*sequence_number <= *old->sequence) return;
            if (*sequence_number != *old->sequence + 1) healthy = false;
            if (old->sequence_gap) healthy = false;
        }
        Decimal bid, ask, size;
        std::vector<Decimal> bid_levels, ask_levels;
        try {
            bid = parse_input_decimal(update.bid, "bid", true);
            ask = parse_input_decimal(update.ask, "ask", true);
            size = parse_input_decimal(update.bid_size, "bid_size", false, true);
            if (update.bid_prices) {
                for (const json& level : *update.bid_prices) bid_levels.push_back(parse_input_decimal(level, "bid level", true));
            } else {
                bid_levels.push_back(bid);
            }
            if (update.ask_prices) {
                for (const json& level : *update.ask_prices) ask_levels.push_back(parse_input_decimal(level, "ask level", true));
            } else {
                ask_levels.push_back(ask);
            }
            std::sort(bid_levels.begin(), bid_levels.end(), [](const Decimal& a, const Decimal& b) { return a > b; });
            bid_levels.erase(std::unique(bid_levels.begin(), bid_levels.end()), bid_levels.end());
            std::sort(ask_levels.begin(), ask_levels.end());
            ask_levels.erase(std::unique(ask_levels.begin(), ask_levels.end()), ask_levels.end());
            if (ask < bid) healthy = false;
        } catch (const ValueError&) {
            bid = Decimal::zero();
            ask = Decimal::zero();
            size = Decimal::zero();
            bid_levels.clear();
            ask_levels.clear();
            healthy = false;
        }
        std::optional<double> source = parse_timestamp(update.source_time);
        Decimal consumed;
        std::optional<double> consumed_wall;
        const Decimal& liquidity_price = liquidity_is_ask() ? ask : bid;
        // A missing, empty or zero ask size counts as no displayed size; a negative one is an error.
        Decimal ask_size = json_python_truthy(&update.ask_size) ? parse_decimal(update.ask_size, "ask_size", false, true)
                                                                : Decimal::zero();
        if (old && (liquidity_is_ask() ? old->ask : old->bid) == liquidity_price && old->consumed_wall) {
            if (update.preserve_consumption || !source || *source <= *old->consumed_wall) {
                consumed = old->consumed;
                consumed_wall = old->consumed_wall;
            }
        }
        QuoteState state;
        state.bid = bid;
        state.ask = ask;
        state.bid_size = size;
        state.ask_size = ask_size;
        state.bid_prices = std::move(bid_levels);
        state.ask_prices = std::move(ask_levels);
        state.consumed = consumed;
        state.consumed_wall = consumed_wall;
        state.received_at = now;
        state.source_time = source;
        state.sequence = sequence_number;
        state.healthy = healthy;
        state.accessible = update.accessible;
        state.sequence_gap = old && sequence_number && old->sequence && !update.reset_sequence &&
                             (*sequence_number != *old->sequence + 1 || old->sequence_gap);
        state.version = old ? old->version + 1 : 1;
        quotes_[symbol] = state;
        if (healthy && update.accessible && bid.is_positive()) {
            for (json& parent : parents) {
                Decimal floor = parent.at("minimum_price").is_null() ? Decimal::zero() : D(parent, "minimum_price");
                Decimal price = Decimal::max(bid, floor).round_up_to_step(D(parent, "tick_size"));
                Decimal legal_minimum = Decimal::max(
                    D(parent, "min_quantity"), D(parent, "min_notional").div(price, 30).round_up_to_step(D(parent, "quantity_step")));
                Decimal base = parent.contains("base_child_cap") ? D(parent, "base_child_cap") : D(parent, "child_cap");
                parent["child_cap"] = S(Decimal::max(base, legal_minimum));
                trim(parent);
                if (source) latency(parent, "feed", std::max(0.0, wall_clock_() - *source));
                save(parent);
            }
        }
    });
}

void SmartSellingEngine::set_feed_health(const std::string& symbol_in, bool healthy, const std::string& reason,
                                         bool reset_sequence) {
    std::string symbol = to_upper(symbol_in);
    transaction([&] {
        auto it = quotes_.find(symbol);
        if (it != quotes_.end()) {
            it->second.healthy = healthy;
            if (healthy) it->second.received_at = clock_();
            if (reset_sequence) it->second.sequence_gap = false;
        }
        for (json& parent : active(symbol)) {
            if (reset_sequence) {
                parent["tape_gap"] = false;
                parent["trade_sequence"] = nullptr;
            }
            if (!healthy) {
                parent["reason"] = reason.empty() ? "feed disconnected" : reason;
                if (!parent.at("order").is_null()) parent["order"]["cancel_intent"] = true;
            }
            save(parent);
            event(parent, "feed_health", json{{"healthy", healthy}, {"reason", reason}});
        }
    });
}

void SmartSellingEngine::on_trade(const std::string& symbol_in, const TradeUpdate& trade) {
    std::string symbol = to_upper(symbol_in);
    if (trade.event_id.empty()) throw ValueError("a stable tape event_id is required");
    Decimal quantity = parse_input_decimal(trade.quantity, "trade quantity");
    if (!trade.correction_of && quantity <= Decimal::zero()) throw ValueError("trade quantity must be positive");
    double now = trade.received_at ? *trade.received_at : clock_();
    double wall = wall_clock_();
    std::optional<double> source = parse_timestamp(trade.source_time);
    std::optional<double> interval_start = parse_timestamp(trade.interval_start);
    transaction([&] {
        for (json parent : tape_parents(symbol, trade.correction_of)) {
            Statement dup(*db_, "SELECT 1 FROM smart_tape WHERE parent_id=? AND event_id=?");
            dup.bind(1, parent.at("parent_id").get<std::string>());
            dup.bind(2, trade.event_id);
            if (dup.step()) continue;
            std::optional<Decimal> rate;
            if (!trade.correction_of) {
                rate = new_trade_rate(parent, trade.event_id, now, wall, source, trade.sequence, trade.healthy, interval_start);
            } else {
                rate = correction_rate(parent, trade.event_id, *trade.correction_of, quantity);
            }
            if (rate) credit_tape(parent, trade.event_id, quantity, *rate, trade.correction_of, source, now);
        }
    });
}

std::vector<json> SmartSellingEngine::tape_parents(const std::string& symbol, const std::optional<std::string>& correction_of) {
    std::vector<json> parents = active(symbol);
    if (!correction_of) return parents;
    std::map<std::string, json> historical;
    for (auto& p : parents) historical[p.at("parent_id").get<std::string>()] = p;
    Statement found(*db_, "SELECT DISTINCT parent_id FROM smart_tape WHERE event_id=? AND original_id IS NULL");
    found.bind(1, *correction_of);
    std::vector<std::string> ids;
    while (found.step()) ids.push_back(found.column_text(0));
    for (const auto& id : ids) {
        json parent = load(id);
        if (parent.at("symbol").get<std::string>() == symbol) historical[id] = parent;
    }
    std::vector<json> out;
    // Preserve the active-parents ordering first, then historical additions.
    for (auto& p : parents) {
        out.push_back(historical.at(p.at("parent_id").get<std::string>()));
        historical.erase(p.at("parent_id").get<std::string>());
    }
    for (auto& [id, p] : historical) out.push_back(p);
    return out;
}

std::optional<Decimal> SmartSellingEngine::new_trade_rate(json& parent, const std::string& event_id, double now, double wall,
                                                          const std::optional<double>& source,
                                                          const std::optional<long long>& sequence, bool healthy,
                                                          const std::optional<double>& interval_start) {
    if (parent.at("stop_requested").get<bool>() || now < parent.at("start_mono").get<double>() || deadline_reached(parent, now)) {
        return std::nullopt;
    }
    if (interval_start && *interval_start < parent.at("start_wall").get<double>()) {
        event(parent, "tape_interval_excluded", json{{"event_id", event_id}, {"interval_start", *interval_start}});
        return std::nullopt;
    }
    bool invalid_source = source && (wall - *source > parent.at("trade_max_age").get<double>() || *source > wall + 1 ||
                                     *source < parent.at("start_wall").get<double>());
    if (!healthy || invalid_source) {
        parent["reason"] = "stale or unhealthy tape event excluded";
        save(parent);
        event(parent, "tape_excluded", json{{"event_id", event_id}});
        return std::nullopt;
    }
    if (sequence) {
        std::optional<long long> previous = opt_int(parent.at("trade_sequence"));
        if (previous && *sequence <= *previous) return std::nullopt;
        if (previous && *sequence != *previous + 1) parent["tape_gap"] = true;
        parent["trade_sequence"] = *sequence;
    }
    if (parent.at("tape_gap").get<bool>()) {
        parent["reason"] = "tape sequence gap requires reconciliation";
        save(parent);
        return std::nullopt;
    }
    return D(parent, "participation");
}

std::optional<Decimal> SmartSellingEngine::correction_rate(json& parent, const std::string& event_id,
                                                           const std::string& correction_of, const Decimal& quantity) {
    Statement original(*db_, "SELECT rate,net_quantity FROM smart_tape WHERE parent_id=? AND event_id=? AND original_id IS NULL");
    original.bind(1, parent.at("parent_id").get<std::string>());
    original.bind(2, correction_of);
    if (!original.step()) {
        parent["hold"] = "unknown tape correction requires reconciliation";
        save(parent);
        event(parent, "correction_unknown", json{{"event_id", event_id}, {"original_id", correction_of}});
        return std::nullopt;
    }
    Decimal rate = parse_decimal(original.column_text(0));
    Decimal net = parse_decimal(original.column_text(1));
    Decimal corrected = net + quantity;
    if (corrected.is_negative()) {
        parent["hold"] = "invalid tape correction requires reconciliation";
        save(parent);
        return std::nullopt;
    }
    Statement update(*db_, "UPDATE smart_tape SET net_quantity=? WHERE parent_id=? AND event_id=?");
    update.bind(1, S(corrected));
    update.bind(2, parent.at("parent_id").get<std::string>());
    update.bind(3, correction_of);
    update.exec();
    return rate;
}

void SmartSellingEngine::credit_tape(json& parent, const std::string& event_id, const Decimal& quantity, const Decimal& rate,
                                     const std::optional<std::string>& correction_of, const std::optional<double>& source,
                                     double now) {
    Decimal net_quantity = correction_of ? Decimal::zero() : quantity;
    Statement insert(*db_,
                     "INSERT INTO smart_tape(parent_id,event_id,quantity,rate,net_quantity,original_id) VALUES(?,?,?,?,?,?)");
    insert.bind(1, parent.at("parent_id").get<std::string>());
    insert.bind(2, event_id);
    insert.bind(3, S(quantity));
    insert.bind(4, S(rate));
    insert.bind(5, S(net_quantity));
    insert.bind_optional(6, correction_of);
    insert.exec();
    Decimal earned = rate * quantity;
    parent["E"] = S(D(parent, "E") + earned);
    parent["C"] = S(D(parent, "C") + earned);
    trim(parent);
    save(parent);
    json detail{{"event_id", event_id}, {"quantity", S(quantity)}, {"rate", S(rate)}, {"source_time", opt_json(source)},
                {"received_at", now}};
    event(parent, correction_of ? "tape_correction" : "tape_credit", detail);
}

json SmartSellingEngine::action_dict(const json& parent, const std::string& kind) {
    const json& order = parent.at("order");
    json action;
    action["type"] = kind;
    action["parent_id"] = parent.at("parent_id");
    action["account_id"] = order.at("account_id");
    action["symbol"] = parent.at("symbol");
    action["client_order_id"] = order.at("client_order_id");
    action["broker_order_id"] = order.at("broker_order_id");
    if (kind == "submit") {
        action["quantity"] = order.at("quantity");
        action["limit_price"] = order.at("limit_price");
        action["side"] = side();
        action["order_type"] = "LIMIT";
        action["time_in_force"] = parent.at("time_in_force");
        action["send_time"] = iso_utc(order.at("send_wall").get<double>());
        action["send_timestamp"] = order.at("send_wall");
        auto deadline = utc_timestamp_string(opt_double(parent.at("deadline_wall")));
        action["deadline"] = deadline ? json(*deadline) : json(nullptr);
    } else if (kind == "query") {
        action["include_open_orders_and_executions"] = true;
    }
    return action;
}

void SmartSellingEngine::action(json& parent, const std::string& kind) {
    json a = action_dict(parent, kind);
    Statement insert(*db_, "INSERT INTO smart_outbox(data) VALUES(?)");
    insert.bind(1, json_dump_compact(a));
    insert.exec();
    event(parent, kind + "_intent", a);
}

std::optional<json> SmartSellingEngine::prepare_dispatch(const std::string& client_order_id) {
    double now = clock_();
    return transaction([&]() -> std::optional<json> {
        Statement row(*db_, "SELECT parent_id FROM smart_orders WHERE id=?");
        row.bind(1, client_order_id);
        if (!row.step()) return std::nullopt;
        json parent = load(row.column_text(0));
        if (parent.at("order").is_null()) return std::nullopt;
        json& order = parent["order"];
        if (!parent.at("active").get<bool>() || order.at("client_order_id").get<std::string>() != client_order_id ||
            order.value("dispatch_prepared", false) || !order.at("ack_at").is_null() || order.at("accepted").get<bool>() ||
            !order.at("sequence").is_null() || !order.at("broker_order_id").is_null() ||
            !D(order, "cumulative_filled").is_zero()) {
            return std::nullopt;
        }
        if (order.at("state").get<std::string>() == "RECONCILE" && order.value("reconcile_cause", json(nullptr)) == json("timer")) {
            abort_unsent(parent, order, "submission queue exceeded acknowledgement wait", now);
            return std::nullopt;
        }
        if (order.at("state").get<std::string>() != "SUBMIT_PENDING") return std::nullopt;
        finish_if_needed(parent, now);
        std::string reason;
        if (parent.at("stop_requested").get<bool>()) reason = parent.at("stop_reason").get<std::string>();
        else {
            reason = parent.at("hold").get<std::string>();
            if (reason.empty()) reason = quote_reason(parent, now);
        }
        if (reason.empty() && D(parent, "C").is_negative()) reason = "negative credit before dispatch";
        if (reason.empty()) reason = price_distance_reason(parent, quotes_.at(parent.at("symbol").get<std::string>()), side());
        if (reason.empty()) {
            const QuoteState& quote = quotes_.at(parent.at("symbol").get<std::string>());
            Decimal old_quantity = D(order, "quantity");
            Decimal step = D(parent, "quantity_step");
            Decimal other_reserved = D(parent, "R") - old_quantity;
            Decimal remaining = D(parent, "quantity") - D(parent, "F") - other_reserved;
            std::optional<Decimal> depth = depth_budget(quote);
            auto [price, aggressive] = dispatch_price(parent, order, quote);
            std::string order_account = order.at("account_id").get<std::string>();
            json& account = parent["accounts"][order_account];
            Decimal account_remaining = account_capacity(parent, order_account, price, true);
            Decimal size_cap = parent.at("disable_child_cap").get<bool>() ? remaining : D(parent, "child_cap");
            Decimal quantity = Decimal::min(Decimal::min(Decimal::min(old_quantity, size_cap), remaining),
                                            Decimal::min(account_remaining, depth ? *depth : remaining))
                                   .round_down_to_step(step);
            int age_limit = aggressive ? 2 : 5;
            if (!meets_order_minimum(parent, quantity, price)) {
                reason = "available quantity below broker minimum before dispatch";
            } else if (now - order.at("send_mono").get<double>() >= age_limit) {
                reason = "submission queue exceeded child cancellation age";
            } else {
                Decimal refund = old_quantity - quantity;
                parent["C"] = S(D(parent, "C") + refund);
                parent["R"] = S(D(parent, "R") - refund);
                account["R"] = S(D(account, "R") - refund);
                order["quantity"] = S(quantity);
                order["limit_price"] = S(price);
                order["aggressive"] = aggressive;
                order["age_limit"] = age_limit;
                order["dispatch_prepared"] = true;
                order["quote_version"] = quote.version;
                order["quote_bid"] = S(quote.bid);
                order["quote_ask"] = S(quote.ask);
                order["quote_bid_size"] = S(quote.bid_size);
                order["quote_source_time"] = opt_json(quote.source_time);
                order["quote_received_at"] = quote.received_at;
                trim(parent);
                save(parent);
                json a = action_dict(parent, "submit");
                event(parent, "submit_dispatch_prepared", a);
                return a;
            }
        }
        abort_unsent(parent, order, reason, now);
        return std::nullopt;
    });
}

void SmartSellingEngine::abort_unsent(json& parent, json& order, const std::string& reason, double now, bool stop_parent) {
    // Callers have proved that this child was never handed to the network.
    Decimal refund = D(order, "quantity");
    parent["C"] = S(D(parent, "C") + refund);
    parent["R"] = S(D(parent, "R") - refund);
    json& account = parent["accounts"][order.at("account_id").get<std::string>()];
    account["R"] = S(D(account, "R") - refund);
    order["state"] = "TERMINAL";
    order["terminal_at"] = now;
    order["terminal_source"] = "LOCAL_NOT_DISPATCHED";
    save_order(parent.at("parent_id").get<std::string>(), order);
    json archived = order;
    parent["order"] = nullptr;
    parent["state"] = "HOLD";
    parent["reason"] = reason;
    if (stop_parent) {
        parent["stop_requested"] = true;
        parent["stop_reason"] = reason;
    }
    trim(parent);
    finish_if_needed(parent, now);
    save(parent);
    event(parent, "submit_not_dispatched",
          json{{"client_order_id", archived.at("client_order_id")}, {"reason", reason}, {"refund", S(refund)}});
}

bool SmartSellingEngine::abort_dispatch(const std::string& client_order_id, const std::string& reason, bool stop_parent) {
    return transaction([&]() -> bool {
        Statement row(*db_, "SELECT parent_id FROM smart_orders WHERE id=?");
        row.bind(1, client_order_id);
        if (!row.step()) return false;
        json parent = load(row.column_text(0));
        if (parent.at("order").is_null()) return false;
        json& order = parent["order"];
        if (!parent.at("active").get<bool>() || order.at("client_order_id").get<std::string>() != client_order_id ||
            order.at("state").get<std::string>() != "SUBMIT_PENDING" || !order.at("ack_at").is_null() ||
            order.at("accepted").get<bool>() || !order.at("sequence").is_null() || !order.at("broker_order_id").is_null() ||
            !D(order, "cumulative_filled").is_zero()) {
            return false;
        }
        abort_unsent(parent, order, reason, clock_(), stop_parent);
        return true;
    });
}

bool SmartSellingEngine::hold_order(const std::string& client_order_id, const std::string& reason) {
    return transaction([&]() -> bool {
        Statement row(*db_, "SELECT parent_id FROM smart_orders WHERE id=?");
        row.bind(1, client_order_id);
        if (!row.step()) return false;
        json parent = load(row.column_text(0));
        hold_conflict(parent, reason);
        return true;
    });
}

std::vector<json> SmartSellingEngine::drain_actions() {
    return transaction([&]() -> std::vector<json> {
        std::vector<json> out;
        Statement rows(*db_, "SELECT id,data FROM smart_outbox WHERE dispatched=0 ORDER BY id");
        while (rows.step()) {
            json a = json::parse(rows.column_text(1));
            a["action_id"] = rows.column_int(0);
            out.push_back(std::move(a));
        }
        db_->exec("UPDATE smart_outbox SET dispatched=1 WHERE dispatched=0");
        return out;
    });
}

void SmartSellingEngine::query(json& parent, double now) {
    json& order = parent["order"];
    if (order.at("last_query").is_null() || now - order.at("last_query").get<double>() >= 5) {
        order["last_query"] = now;
        action(parent, "query");
    }
}

void SmartSellingEngine::cancel(json& parent, double now) {
    json& order = parent["order"];
    order["cancel_intent"] = true;
    if (order.at("broker_order_id").is_null() || !order.at("cancel_at").is_null()) return;
    if (!order.at("last_cancel").is_null() && now - order.at("last_cancel").get<double>() < 5) return;
    order["cancel_at"] = now;
    order["last_cancel"] = now;
    order["cancel_requested_ever"] = true;
    order["state"] = "CANCEL_PENDING";
    action(parent, "cancel");
}

void SmartSellingEngine::on_action_error(const std::string& client_order_id, const std::string& act, const std::string& error,
                                         bool definitive, std::optional<double> now_in) {
    (void)definitive;
    double now = now_in ? *now_in : clock_();
    transaction([&] {
        Statement row(*db_, "SELECT parent_id FROM smart_orders WHERE id=?");
        row.bind(1, client_order_id);
        if (!row.step()) return;
        json parent = load(row.column_text(0));
        if (parent.at("order").is_null() || parent.at("order").at("client_order_id").get<std::string>() != client_order_id) return;
        json& order = parent["order"];
        order["state"] = "RECONCILE";
        order["reconcile_cause"] = "transport";
        if (act == "cancel") order["cancel_failed"] = true;
        parent["reason"] = act + " uncertain: " + error.substr(0, 240);
        event(parent, "action_error", json{{"action", act}, {"message", error.substr(0, 500)}});
        query(parent, now);
        save(parent);
    });
}

void SmartSellingEngine::on_order_report(const std::string& client_order_id, const OrderReport& report) {
    double now = report.now ? *report.now : clock_();
    std::string status = to_upper(report.status);
    Decimal cumulative = parse_input_decimal(report.cumulative_filled, "cumulative_filled", false, true);
    transaction([&] {
        Statement row(*db_, "SELECT parent_id,data FROM smart_orders WHERE id=?");
        row.bind(1, client_order_id);
        if (!row.step()) return;
        json parent = load(row.column_text(0));
        json archived = json::parse(row.column_text(1));
        if (parent.at("order").is_null() || parent.at("order").at("client_order_id").get<std::string>() != client_order_id) {
            check_archived_report(parent, archived, status, cumulative, report.sequence, report.authoritative);
            return;
        }
        json& order = parent["order"];
        if (report.broker_order_id) {
            const json& current = order.at("broker_order_id");
            if (!current.is_null() && current.get<std::string>() != *report.broker_order_id) {
                hold_conflict(parent, "broker order ID conflict requires reconciliation");
                return;
            }
            order["broker_order_id"] = *report.broker_order_id;
        }
        if (!report.authoritative) {
            order["state"] = "RECONCILE";
            order["reconcile_cause"] = "order_report";
            query(parent, now);
            save(parent);
            return;
        }
        if (!accept_cumulative_report(parent, order, cumulative, report.sequence)) return;
        book_cumulative_fill(parent, order, cumulative);
        if (report.sequence) order["sequence"] = *report.sequence;
        acknowledge_order(parent, order, status, cumulative, now);
        bool final = report.final;
        if (status == "FILLED" && cumulative != D(order, "quantity")) final = false;
        if (is_terminal_status(status) && final) {
            apply_terminal_report(parent, order, status, cumulative, now);
        } else if (is_terminal_status(status) || status == "UNKNOWN" || status == "NOT_FOUND") {
            order["state"] = "RECONCILE";
            order["reconcile_cause"] = "order_report";
            parent["reason"] = "authoritative final cumulative fill required";
            query(parent, now);
        } else if (is_working_status(status)) {
            apply_working_report(parent, order, now);
        }
        save(parent);
        json detail{{"client_order_id", client_order_id}, {"status", status}, {"cumulative_filled", S(cumulative)},
                    {"sequence", report.sequence ? json(*report.sequence) : json(nullptr)}, {"final", final}};
        event(parent, "order_report", detail);
    });
}

void SmartSellingEngine::check_archived_report(json& parent, const json& archived, const std::string& status,
                                               const Decimal& cumulative, const std::optional<long long>& sequence,
                                               bool authoritative) {
    Decimal previous_cumulative = D(archived, "cumulative_filled");
    if (!authoritative) return;
    std::optional<long long> previous_sequence = opt_int(archived.value("sequence", json(nullptr)));
    bool newer_report = sequence && previous_sequence && *sequence > *previous_sequence;
    bool working_again = is_working_status(status) || status == "PENDING_NEW";
    if (newer_report && working_again) {
        hold_conflict(parent, "archived order reported working after terminal status");
        return;
    }
    if (cumulative == previous_cumulative) return;
    if (cumulative > previous_cumulative || newer_report) {
        hold_conflict(parent, "conflicting archived fill requires reconciliation");
    }
}

bool SmartSellingEngine::accept_cumulative_report(json& parent, json& order, const Decimal& cumulative,
                                                  const std::optional<long long>& sequence) {
    Decimal previous_cumulative = D(order, "cumulative_filled");
    std::optional<long long> previous_sequence = opt_int(order.at("sequence"));
    if (sequence && previous_sequence && *sequence <= *previous_sequence) {
        if (cumulative > previous_cumulative) hold_conflict(parent, "conflicting reordered fill requires reconciliation");
        return false;
    }
    if (cumulative < previous_cumulative) {
        if (sequence && previous_sequence && *sequence > *previous_sequence) {
            hold_conflict(parent, "execution bust requires reconciliation");
        }
        return false;
    }
    if (cumulative > D(order, "quantity")) {
        hold_conflict(parent, "cumulative fill exceeds child quantity");
        return false;
    }
    return true;
}

void SmartSellingEngine::book_cumulative_fill(json& parent, json& order, const Decimal& cumulative) {
    Decimal delta = cumulative - D(order, "cumulative_filled");
    if (delta.is_zero()) return;
    parent["F"] = S(D(parent, "F") + delta);
    parent["R"] = S(D(parent, "R") - delta);
    json& account = parent["accounts"][order.at("account_id").get<std::string>()];
    account["F"] = S(D(account, "F") + delta);
    account["R"] = S(D(account, "R") - delta);
    order["cumulative_filled"] = S(cumulative);
    if (order.value("cancel_requested_ever", false) || !order.at("cancel_at").is_null()) {
        parent["fills_after_cancel"] = S(D(parent, "fills_after_cancel") + delta);
    }
    auto it = quotes_.find(parent.at("symbol").get<std::string>());
    bool ask_side = liquidity_is_ask();
    if (it != quotes_.end() && order.at("aggressive").get<bool>() &&
        (ask_side ? it->second.ask : it->second.bid) == D(order, ask_side ? "quote_ask" : "quote_bid")) {
        it->second.consumed += delta;
        it->second.consumed_wall = wall_clock_();
    }
}

void SmartSellingEngine::acknowledge_order(json& parent, json& order, const std::string& status, const Decimal& cumulative,
                                           double now) {
    bool accepted = order.at("accepted").get<bool>() || is_working_status(status) || status == "FILLED" ||
                    status == "CANCELED" || status == "CANCELLED" || status == "EXPIRED" || cumulative.is_positive();
    order["accepted"] = accepted;
    if (order.at("ack_at").is_null() && accepted) {
        order["ack_at"] = now;
        double lat = std::max(0.0, now - order.at("send_mono").get<double>());
        latency(parent, "send_to_ack", lat);
        event(parent, "ack", json{{"latency_seconds", lat}});
    }
}

void SmartSellingEngine::apply_terminal_report(json& parent, json& order, const std::string& status, const Decimal& cumulative,
                                               double now) {
    Decimal refund = D(order, "quantity") - cumulative;
    parent["R"] = S(D(parent, "R") - refund);
    parent["C"] = S(D(parent, "C") + refund);
    json& account = parent["accounts"][order.at("account_id").get<std::string>()];
    account["R"] = S(D(account, "R") - refund);
    if (status == "REJECTED") {
        parent["stop_requested"] = true;
        parent["stop_reason"] = "broker rejected child; review required";
    } else if (order.at("accepted").get<bool>()) {
        parent["fallback"] = !(order.at("aggressive").get<bool>() || refund.is_zero());
    }
    order["state"] = "TERMINAL";
    order["terminal_at"] = now;
    json cancel_latency = nullptr;
    if (!order.at("last_cancel").is_null()) {
        double lat = std::max(0.0, now - order.at("last_cancel").get<double>());
        latency(parent, "cancel_to_terminal", lat);
        cancel_latency = lat;
    }
    save_order(parent.at("parent_id").get<std::string>(), order);
    json archived = order;
    parent["order"] = nullptr;
    trim(parent);
    finish_if_needed(parent, now);
    event(parent, "terminal",
          json{{"client_order_id", archived.at("client_order_id")}, {"status", status}, {"cumulative_filled", S(cumulative)},
               {"refund", S(refund)}, {"cancel_to_terminal", cancel_latency}});
}

void SmartSellingEngine::apply_working_report(json& parent, json& order, double now) {
    if (order.at("state").get<std::string>() == "RECONCILE") {
        if (order.at("cancel_intent").get<bool>() && !order.at("last_cancel").is_null() &&
            now - order.at("last_cancel").get<double>() >= 5) {
            order["cancel_at"] = nullptr;
        }
    }
    if (order.at("cancel_at").is_null()) order["state"] = "LIVE";
    if (order.at("cancel_intent").get<bool>() || now - order.at("send_mono").get<double>() >= order.at("age_limit").get<double>()) {
        cancel(parent, now);
    }
}

void SmartSellingEngine::hold_conflict(json& parent, const std::string& reason) {
    parent["hold"] = reason;
    if (parent.at("active").get<bool>()) parent["state"] = parent.at("stop_requested").get<bool>() ? "STOPPING" : "HOLD";
    if (!parent.at("order").is_null()) {
        parent["order"]["cancel_intent"] = true;
        parent["order"]["state"] = "RECONCILE";
        parent["order"]["reconcile_cause"] = "conflict";
    }
    save(parent);
    event(parent, "order_conflict", json{{"reason", reason}});
    std::vector<std::string> mine = parent.at("account_ids").get<std::vector<std::string>>();
    for (json current : active(parent.at("symbol").get<std::string>())) {
        if (current.at("parent_id") == parent.at("parent_id")) continue;
        bool intersects = false;
        for (const auto& id : current.at("account_ids")) {
            if (std::find(mine.begin(), mine.end(), id.get<std::string>()) != mine.end()) intersects = true;
        }
        if (!intersects) continue;
        current["hold"] = reason;
        if (!current.at("order").is_null()) current["order"]["cancel_intent"] = true;
        save(current);
    }
}

json SmartSellingEngine::request_stop(const std::string& parent_id, std::optional<double> now_in, const std::string& reason) {
    double now = now_in ? *now_in : clock_();
    return transaction([&]() -> json {
        json parent = load(parent_id);
        parent["stop_requested"] = true;
        parent["stop_reason"] = reason;
        if (!parent.at("order").is_null()) cancel(parent, now);
        finish_if_needed(parent, now);
        save(parent);
        event(parent, "stop_requested", json{{"reason", reason}});
        return make_public(parent);
    });
}

void SmartSellingEngine::set_remaining_volume_forecast(const std::string& parent_id, const std::optional<Decimal>& quantity) {
    transaction([&] {
        json parent = load(parent_id);
        if (quantity) {
            if (quantity->is_negative()) throw ValueError("invalid remaining volume");
            parent["remaining_volume_forecast"] = S(*quantity);
        } else {
            parent["remaining_volume_forecast"] = nullptr;
        }
        parent["last_diagnostic"] = nullptr;
        save(parent);
    });
}

bool SmartSellingEngine::deadline_reached(const json& parent, double now) {
    return !parent.at("deadline_mono").is_null() && now >= parent.at("deadline_mono").get<double>();
}

void SmartSellingEngine::finish_if_needed(json& parent, double now) {
    if (deadline_reached(parent, now)) {
        parent["stop_requested"] = true;
        parent["stop_reason"] = "deadline reached";
    }
    if (!parent.at("order").is_null()) {
        if (parent.at("stop_requested").get<bool>()) {
            parent["state"] = "STOPPING";
            parent["reason"] = parent.at("stop_reason");
        }
        return;
    }
    if (D(parent, "F") == D(parent, "quantity")) {
        parent["state"] = "COMPLETED";
        parent["reason"] = "parent fully filled";
        parent["active"] = false;
    } else if (parent.at("stop_requested").get<bool>()) {
        parent["state"] = parent.at("stop_reason").get<std::string>() == "deadline reached" ? "DEADLINE" : "STOPPED";
        parent["reason"] = parent.at("stop_reason");
        parent["active"] = false;
    }
}

std::string SmartSellingEngine::quote_reason(const json& parent, double now) {
    auto it = quotes_.find(parent.at("symbol").get<std::string>());
    if (it == quotes_.end()) return "market quote unavailable";
    const QuoteState& quote = it->second;
    if (quote.sequence_gap || parent.at("tape_gap").get<bool>()) return "market data sequence gap requires reconciliation";
    if (!quote.healthy || !quote.accessible) return "market feed unhealthy or depth inaccessible";
    if (now - quote.received_at > parent.at("quote_max_age").get<double>()) return "market quote heartbeat stale";
    if (quote.source_time) {
        double wall = wall_clock_();
        if (wall - *quote.source_time > parent.at("quote_max_age").get<double>() || *quote.source_time > wall + 1) {
            return "market quote source timestamp stale";
        }
    }
    return "";
}

void SmartSellingEngine::latency(json& parent, const std::string& kind, double seconds) {
    if (!parent.contains("latencies") || !parent["latencies"].is_object()) parent["latencies"] = json::object();
    json& metrics = parent["latencies"];
    if (!metrics.contains(kind)) metrics[kind] = json{{"count", 0}, {"samples", json::array()}, {"observed_max", 0}};
    json& metric = metrics[kind];
    metric["count"] = metric.at("count").get<long long>() + 1;
    json samples = metric.at("samples");
    samples.push_back(seconds);
    while (samples.size() > 128) samples.erase(samples.begin());
    metric["samples"] = samples;
    double observed = metric.at("observed_max").get<double>();
    metric["observed_max"] = std::max(observed, seconds);
    std::vector<double> sorted;
    for (const auto& s : samples) sorted.push_back(s.get<double>());
    std::sort(sorted.begin(), sorted.end());
    std::size_t n = sorted.size();
    double median = n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    metric["median"] = median;
    metric["sample_window"] = 128;
}

void SmartSellingEngine::diagnostic(json& parent, double now) {
    if (!parent.at("last_diagnostic").is_null() && now - parent.at("last_diagnostic").get<double>() < 30) return;
    Decimal remaining = D(parent, "quantity") - D(parent, "F");
    Decimal unfunded = Decimal::max(Decimal::zero(), remaining - D(parent, "C") - D(parent, "R"));
    json scenarios = nullptr;
    const json& forecast = parent.at("remaining_volume_forecast");
    if (!forecast.is_null()) {
        Decimal rate = D(parent, "participation");
        Decimal fc = D(forecast);
        scenarios = json::object();
        for (auto [label, factor] : {std::pair{"base", Decimal(1)}, std::pair{"75_percent", Decimal::parse(".75")},
                                     std::pair{"50_percent", Decimal::parse(".5")}}) {
            scenarios[label] = json{{"eligible_volume", S(fc * factor)}, {"credit_shortfall", unfunded > rate * fc * factor}};
        }
    }
    json capacity;
    capacity["remaining"] = S(remaining);
    capacity["unfunded"] = S(unfunded);
    capacity["required_future_volume"] = S(unfunded.div(D(parent, "participation"), 30));
    capacity["scenarios"] = scenarios;
    capacity["forecast_available"] = !forecast.is_null();
    capacity["depth_cap_count"] = parent.at("depth_cap_count");
    capacity["loop_lag_seconds"] =
        parent.at("last_tick").is_null() ? 0.0 : std::max(0.0, now - parent.at("last_tick").get<double>() - 1);
    parent["capacity"] = capacity;
    parent["last_diagnostic"] = now;
    event(parent, "capacity", capacity);
}

std::vector<json> SmartSellingEngine::tick(std::optional<double> now_in) {
    double now = now_in ? *now_in : clock_();
    transaction([&] {
        for (json parent : active()) tick_parent(parent, now);
        std::set<std::string> active_symbols;
        for (const json& parent : active()) active_symbols.insert(parent.at("symbol").get<std::string>());
        for (auto it = quotes_.begin(); it != quotes_.end();) {
            if (!active_symbols.count(it->first)) it = quotes_.erase(it);
            else ++it;
        }
    });
    return drain_actions();
}

void SmartSellingEngine::tick_parent(json& parent, double now) {
    if (!parent.at("last_tick").is_null()) latency(parent, "loop_lag", std::max(0.0, now - parent.at("last_tick").get<double>() - 1));
    diagnostic(parent, now);
    parent["last_tick"] = now;
    finish_if_needed(parent, now);
    if (!parent.at("order").is_null()) {
        service_working_order(parent, now);
    } else if (parent.at("active").get<bool>()) {
        auto plan = plan_child(parent, now);
        if (plan) {
            reserve_child(parent, *plan, now);
            return;
        }
    }
    save(parent);
}

void SmartSellingEngine::service_working_order(json& parent, double now) {
    json& order = parent["order"];
    std::string quote_invalid = quote_reason(parent, now);
    bool cancellation_due = now - order.at("send_mono").get<double>() >= order.at("age_limit").get<double>();
    if (parent.at("stop_requested").get<bool>() || !parent.at("hold").get<std::string>().empty() || !quote_invalid.empty() ||
        cancellation_due) {
        cancel(parent, now);
    }
    if (!order.at("cancel_at").is_null()) order["cancel_requested_ever"] = true;
    bool submit_unresolved = order.at("ack_at").is_null() && now - order.at("send_mono").get<double>() >= 3;
    bool cancel_unresolved = !order.at("cancel_at").is_null() && now - order.at("cancel_at").get<double>() >= 3;
    if (submit_unresolved || cancel_unresolved || order.at("state").get<std::string>() == "RECONCILE") {
        if (order.at("state").get<std::string>() != "RECONCILE") order["reconcile_cause"] = "timer";
        order["state"] = "RECONCILE";
        query(parent, now);
    }
    if (parent.at("stop_requested").get<bool>()) return;
    bool held = !parent.at("hold").get<std::string>().empty();
    bool reconcile = order.at("state").get<std::string>() == "RECONCILE";
    parent["state"] = (held || reconcile) ? "HOLD" : "RUNNING";
    if (held) parent["reason"] = parent.at("hold");
    else if (reconcile) parent["reason"] = "order reconciliation required";
    else parent["reason"] = "waiting for child terminal status";
}

std::optional<ChildPlan> SmartSellingEngine::plan_child(json& parent, double now) {
    if (now < parent.at("start_mono").get<double>()) {
        parent["state"] = "SCHEDULED";
        parent["reason"] = "waiting for parent start";
        return std::nullopt;
    }
    std::string reason = parent.at("hold").get<std::string>();
    if (reason.empty()) reason = quote_reason(parent, now);
    if (reason.empty()) reason = price_distance_reason(parent, quotes_.at(parent.at("symbol").get<std::string>()), side());
    if (!reason.empty() || D(parent, "C").is_negative()) {
        parent["state"] = "HOLD";
        parent["reason"] = reason.empty() ? "negative credit after tape correction" : reason;
        return std::nullopt;
    }
    parent["state"] = "RUNNING";
    if (!parent.at("last_send").is_null() && now - parent.at("last_send").get<double>() < 5) {
        parent["reason"] = "minimum five-second send spacing";
        return std::nullopt;
    }
    const QuoteState& quote = quotes_.at(parent.at("symbol").get<std::string>());
    Decimal continuation = D(parent, "slice_remaining");
    ChildPlan plan;
    if (!continuation.is_zero()) {
        json sizing = parent;
        sizing["child_cap"] = S(Decimal::min(continuation, D(parent, "child_cap")));
        plan = child_policy(sizing, quote, now);
        if (!meets_order_minimum(parent, continuation, plan.limit_price)) {
            // A split remainder below the broker minimum joins future volume credit; it must never
            // become an illegal child on another account.
            parent["slice_remaining"] = "0";
            plan = child_policy(parent, quote, now);
        }
    } else {
        plan = child_policy(parent, quote, now);
    }
    if (plan.depth_limited) parent["depth_cap_count"] = parent.at("depth_cap_count").get<long long>() + 1;
    if (!plan.quantity) {
        parent["reason"] = plan.reason;
        return std::nullopt;
    }
    return plan;
}

void SmartSellingEngine::reserve_child(json& parent, const ChildPlan& plan, double now) {
    Decimal quantity = *plan.quantity;
    const QuoteState& quote = quotes_.at(parent.at("symbol").get<std::string>());
    std::vector<std::string> account_ids = parent.at("account_ids").get<std::vector<std::string>>();
    std::size_t index = 0;
    std::string account_id;
    Decimal allocated;
    bool found = false;
    for (std::size_t candidate : account_selection_indices(parent)) {
        index = candidate;
        account_id = account_ids[index];
        Decimal available = account_capacity(parent, account_id, plan.limit_price);
        allocated = Decimal::min(quantity, available).round_down_to_step(D(parent, "quantity_step"));
        if (meets_order_minimum(parent, allocated, plan.limit_price)) {
            found = true;
            break;
        }
    }
    if (!found) {
        parent["state"] = "HOLD";
        parent["reason"] = "remaining account inventory below broker minimum";
        save(parent);
        return;
    }
    Decimal slice_remaining = D(parent, "slice_remaining");
    Decimal slice_quantity = slice_remaining.is_zero() ? quantity : slice_remaining;
    parent["slice_remaining"] = S(Decimal::max(Decimal::zero(), slice_quantity - allocated));
    parent["next_account_index"] = static_cast<long long>((index + 1) % account_ids.size());
    quantity = allocated;
    json& account = parent["accounts"][account_id];
    account["R"] = S(D(account, "R") + quantity);
    double wall = wall_clock_();
    parent["C"] = S(D(parent, "C") - quantity);
    parent["R"] = S(D(parent, "R") + quantity);
    parent["last_send"] = now;
    parent["last_send_wall"] = wall;
    json order;
    order["account_id"] = account_id;
    order["client_order_id"] = "smart-" + uuid_hex();
    order["broker_order_id"] = nullptr;
    order["state"] = "SUBMIT_PENDING";
    order["dispatch_prepared"] = false;
    order["reconcile_cause"] = nullptr;
    order["quantity"] = S(quantity);
    order["limit_price"] = S(plan.limit_price);
    order["cumulative_filled"] = "0";
    order["sequence"] = nullptr;
    order["aggressive"] = plan.aggressive;
    order["age_limit"] = plan.aggressive ? 2 : 5;
    order["send_mono"] = now;
    order["send_wall"] = wall;
    order["ack_at"] = nullptr;
    order["accepted"] = false;
    order["cancel_intent"] = false;
    order["cancel_at"] = nullptr;
    order["last_cancel"] = nullptr;
    order["last_query"] = nullptr;
    order["cancel_failed"] = false;
    order["cancel_requested_ever"] = false;
    order["quote_version"] = quote.version;
    order["quote_bid"] = S(quote.bid);
    order["quote_ask"] = S(quote.ask);
    order["quote_bid_size"] = S(quote.bid_size);
    order["quote_source_time"] = opt_json(quote.source_time);
    order["quote_received_at"] = quote.received_at;
    parent["order"] = order;
    if (std::string(side()) == "SELL" && plan.aggressive && plan.limit_price > quote.bid) {
        parent["reason"] = "minimum price blocks immediate execution";
    }
    else if (plan.aggressive) parent["reason"] = "aggressive child submitted";
    else parent["reason"] = "passive child submitted";
    save(parent);
    action(parent, "submit");
}

json SmartSellingEngine::make_public(json parent) {
    parent = upgrade_parent(std::move(parent));
    json out;
    out["parent_id"] = parent.at("parent_id");
    out["account_id"] = parent.at("account_id");
    out["account_ids"] = parent.at("account_ids");
    json accounts = json::object();
    for (auto it = parent.at("accounts").begin(); it != parent.at("accounts").end(); ++it) {
        const json& a = it.value();
        accounts[it.key()] = json{{"quantity", a.at("quantity")},
                                  {"average_cost", a.value("average_cost", json(nullptr))},
                                  {"filled", a.at("F")},
                                  {"reserved", a.at("R")},
                                  {"remaining", S(D(a, "quantity") - D(a, "F"))},
                                  {"done", D(a, "quantity") == D(a, "F")}};
    }
    out["accounts"] = accounts;
    out["symbol"] = parent.at("symbol");
    out["quantity"] = parent.at("quantity");
    out["mode"] = parent.at("mode");
    out["participation"] = parent.at("participation");
    out["filled"] = parent.at("F");
    out["remaining"] = S(D(parent, "quantity") - D(parent, "F"));
    out["reserved"] = parent.at("R");
    out["free_credit"] = parent.at("C");
    out["earned"] = parent.at("E");
    out["discarded"] = parent.at("D");
    out["state"] = parent.at("state");
    std::string hold = parent.at("hold").get<std::string>();
    out["reason"] = hold.empty() ? parent.at("reason") : json(hold);
    out["active"] = parent.at("active");
    auto start = utc_timestamp_string(opt_double(parent.at("start_wall")));
    out["start"] = start ? json(*start) : json(nullptr);
    auto deadline = utc_timestamp_string(opt_double(parent.at("deadline_wall")));
    out["deadline"] = deadline ? json(*deadline) : json(nullptr);
    out["minimum_price"] = parent.at("minimum_price");
    out["child_cap"] = parent.at("disable_child_cap").get<bool>() ? json(nullptr) : parent.at("child_cap");
    out["disable_child_cap"] = parent.at("disable_child_cap");
    out["fallback"] = parent.at("fallback");
    out["active_order"] = parent.at("order");
    out["order_state"] = parent.at("order").is_null() ? json("IDLE") : parent.at("order").at("state");
    out["capacity"] = parent.at("capacity");
    out["fills_after_cancel"] = parent.at("fills_after_cancel");
    json latencies = json::object();
    if (parent.contains("latencies") && parent.at("latencies").is_object()) {
        for (auto it = parent.at("latencies").begin(); it != parent.at("latencies").end(); ++it) {
            json metric = it.value();
            metric.erase("samples");
            latencies[it.key()] = metric;
        }
    }
    out["latencies"] = latencies;
    out["residual_provisional"] = !parent.at("order").is_null() || !hold.empty();
    return out;
}

std::optional<json> SmartSellingEngine::get_market_checkpoint(const std::string& symbol) {
    std::lock_guard lock(mutex_);
    if (!db_) return std::nullopt;
    Statement stmt(*db_, "SELECT data FROM smart_market_checkpoints WHERE symbol=?");
    stmt.bind(1, to_upper(ng::trim(symbol)));
    if (!stmt.step()) return std::nullopt;
    return json::parse(stmt.column_text(0));
}

void SmartSellingEngine::set_market_checkpoint(const std::string& symbol, const json& checkpoint) {
    if (!checkpoint.is_object()) throw ValueError("market checkpoint must be an object");
    std::string data = json_dump_compact(checkpoint);
    if (data.size() > 16384) throw ValueError("market checkpoint exceeds size limit");
    transaction([&] {
        Statement stmt(*db_,
                       "INSERT INTO smart_market_checkpoints(symbol,data) VALUES(?,?) "
                       "ON CONFLICT(symbol) DO UPDATE SET data=excluded.data");
        stmt.bind(1, to_upper(ng::trim(symbol)));
        stmt.bind(2, data);
        stmt.exec();
    });
}

std::optional<json> SmartSellingEngine::get_order_context(const std::string& client_order_id) {
    std::lock_guard lock(mutex_);
    if (!db_) return std::nullopt;
    Statement stmt(*db_, "SELECT parent_id,data FROM smart_orders WHERE id=?");
    stmt.bind(1, client_order_id);
    if (!stmt.step()) return std::nullopt;
    json parent = load(stmt.column_text(0));
    json order = json::parse(stmt.column_text(1));
    json out;
    out["parent_id"] = parent.at("parent_id");
    out["account_id"] = order.value("account_id", parent.at("account_id"));
    out["symbol"] = parent.at("symbol");
    out["side"] = side();
    out["quantity"] = order.at("quantity");
    out["limit_price"] = order.at("limit_price");
    out["broker_order_id"] = order.at("broker_order_id");
    out["client_order_id"] = order.at("client_order_id");
    out["sequence"] = order.at("sequence");
    out["cumulative_filled"] = order.at("cumulative_filled");
    out["state"] = order.at("state");
    out["send_time"] = iso_utc(order.at("send_wall").get<double>());
    out["active"] = parent.at("active").get<bool>() && !parent.at("order").is_null() &&
                    parent.at("order").at("client_order_id") == order.at("client_order_id");
    return out;
}

std::optional<json> SmartSellingEngine::find_order_by_broker_id(const std::string& account_id, const std::string& broker_order_id) {
    std::lock_guard lock(mutex_);
    if (!db_) return std::nullopt;
    Statement stmt(*db_,
                   "SELECT child.id FROM smart_orders child "
                   "JOIN smart_parents parent ON parent.id=child.parent_id "
                   "WHERE COALESCE(json_extract(child.data, '$.account_id'), parent.account_id)=? "
                   "AND json_extract(child.data, '$.broker_order_id')=? LIMIT 2");
    stmt.bind(1, account_id);
    stmt.bind(2, broker_order_id);
    std::vector<std::string> ids;
    while (stmt.step()) ids.push_back(stmt.column_text(0));
    if (ids.size() != 1) return std::nullopt;
    return get_order_context(ids[0]);
}

json SmartSellingEngine::get_parent(const std::string& parent_id) {
    std::lock_guard lock(mutex_);
    if (!db_) throw std::runtime_error("Smart selling engine is closed");
    return make_public(load(parent_id));
}

std::vector<json> SmartSellingEngine::list_parents(bool active_only, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<json> out;
    if (!db_) return out;
    std::string sql = "SELECT data FROM smart_parents";
    if (active_only) sql += " WHERE active=1";
    sql += " ORDER BY rowid DESC LIMIT ?";
    Statement stmt(*db_, sql);
    stmt.bind(1, static_cast<long long>(std::max(1, std::min(limit, 1000))));
    while (stmt.step()) out.push_back(make_public(json::parse(stmt.column_text(0))));
    return out;
}

std::vector<json> SmartSellingEngine::events(const std::string& parent_id, int limit) {
    std::lock_guard lock(mutex_);
    std::vector<json> out;
    if (!db_) return out;
    Statement stmt(*db_, "SELECT id,ts,kind,data FROM smart_events WHERE parent_id=? ORDER BY id DESC LIMIT ?");
    stmt.bind(1, parent_id);
    stmt.bind(2, static_cast<long long>(std::max(1, std::min(limit, 1000))));
    while (stmt.step()) {
        json e;
        e["id"] = stmt.column_int(0);
        e["ts"] = iso_utc(stmt.column_double(1));
        e["kind"] = stmt.column_text(2);
        e["data"] = json::parse(stmt.column_text(3));
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace ng::smart
