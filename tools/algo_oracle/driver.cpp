// Scenario driver for differential testing of the algorithm modes against the original Python
// implementation. Reads a JSON scenario on stdin, executes it against the C++ Smart selling
// engine / market adapter / feedback correlator (kind "smart") or the fast-trading cycle
// planner (kind "fast"), and prints a JSON trace on stdout. tools/algo_oracle/py_driver.py
// runs the same scenario against trading_ui; oracle_diff.py compares the traces.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "algo/fast_trading/fast_trading.h"
#include "algo/smart_selling/engine.h"
#include "algo/smart_selling/feedback.h"
#include "algo/smart_selling/market_adapter.h"
#include "core/decimal.h"
#include "core/json.h"
#include "model/account.h"
#include "model/market_data.h"

using ng::json;
using ng::smart::OrderReport;
using ng::smart::QuoteUpdate;
using ng::smart::SmartSellingEngine;
using ng::smart::SmartSellingFeedback;
using ng::smart::SmartSellingMarketAdapter;
using ng::smart::TradeUpdate;

namespace {

constexpr double kEpoch = 1800000000.0;

const char* kParentKeys[] = {"state", "order_state", "active", "mode", "participation", "quantity", "earned", "free_credit",
                             "reserved", "filled", "discarded", "remaining", "fallback", "child_cap", "reason", "deadline",
                             "residual_provisional", "fills_after_cancel", "stop_requested", "symbol", "account_ids",
                             "minimum_price", "imminent_event", "time_in_force"};
const char* kOrderKeys[] = {"account_id", "quantity", "limit_price", "state", "cumulative_filled", "cancel_intent",
                            "broker_order_id", "prepared"};
const char* kActionKeys[] = {"type", "account_id", "symbol", "quantity", "limit_price", "side", "order_type", "time_in_force",
                             "send_time", "deadline", "include_open_orders_and_executions"};

struct Session {
    std::string dir;
    double now = 0.0;
    std::unique_ptr<SmartSellingEngine> engine;
    std::unique_ptr<SmartSellingFeedback> feedback;
    std::unique_ptr<SmartSellingMarketAdapter> market;
    json config;
    std::vector<std::string> parents;         // parent index -> parent id
    std::vector<std::string> orders;          // order index -> client order id
    std::map<std::string, int> order_index;   // client order id -> index
    std::map<std::string, json> submit_by_id; // client order id -> submit action

    double wall() const { return kEpoch + now; }

    void open() {
        engine = std::make_unique<SmartSellingEngine>(dir + "/smart.sqlite3", [this] { return now; }, [this] { return wall(); });
        feedback = std::make_unique<SmartSellingFeedback>(*engine, 10000, dir + "/feedback.sqlite3");
        market = std::make_unique<SmartSellingMarketAdapter>(*engine, [this] { return now; }, [this] { return wall(); },
                                                             config.value("max_source_age", 10.0), config.value("rate_window", 300.0),
                                                             config.value("max_observations", 256), config.value("resume_cooldown", 10.0));
    }
    void close() {
        if (feedback) feedback->close();
        if (engine) engine->close();
        market.reset();
        feedback.reset();
        engine.reset();
    }

    void register_action(const json& action) {
        if (!action.is_object() || !action.contains("client_order_id") || !action.at("client_order_id").is_string()) return;
        std::string id = action.at("client_order_id").get<std::string>();
        if (!order_index.count(id)) {
            order_index[id] = static_cast<int>(orders.size());
            orders.push_back(id);
        }
        if (action.value("type", "") == "submit") submit_by_id[id] = action;
    }

    std::optional<std::string> order_id(const json& ref) const {
        if (!ref.is_number_integer()) return std::nullopt;
        long long k = ref.get<long long>();
        if (k < 0 || k >= static_cast<long long>(orders.size())) return std::nullopt;
        return orders[static_cast<std::size_t>(k)];
    }

    json normalise_action(const json& action) {
        register_action(action);
        json out = json::object();
        for (const char* key : kActionKeys) {
            if (action.contains(key)) out[key] = action.at(key);
        }
        if (action.contains("client_order_id") && action.at("client_order_id").is_string()) {
            out["order"] = order_index.at(action.at("client_order_id").get<std::string>());
        }
        return out;
    }

    json normalise_actions(const std::vector<json>& actions) {
        json out = json::array();
        for (auto& a : actions) out.push_back(normalise_action(a));
        return out;
    }

    json normalise_parent(const json& parent) {
        json out = json::object();
        for (const char* key : kParentKeys) {
            if (parent.contains(key)) out[key] = parent.at(key);
        }
        if (parent.contains("accounts") && parent.at("accounts").is_object()) {
            json accounts = json::object();
            for (auto it = parent.at("accounts").begin(); it != parent.at("accounts").end(); ++it) {
                json a = json::object();
                for (const char* key : {"quantity", "filled", "reserved", "remaining", "done"}) {
                    if (it.value().contains(key)) a[key] = it.value().at(key);
                }
                accounts[it.key()] = a;
            }
            out["accounts"] = accounts;
        }
        if (parent.contains("active_order") && parent.at("active_order").is_object()) {
            const json& order = parent.at("active_order");
            json o = json::object();
            for (const char* key : kOrderKeys) {
                if (order.contains(key)) o[key] = order.at(key);
            }
            if (order.contains("client_order_id") && order.at("client_order_id").is_string()) {
                auto it = order_index.find(order.at("client_order_id").get<std::string>());
                o["order"] = it == order_index.end() ? json(nullptr) : json(it->second);
            }
            out["active_order"] = o;
        } else if (parent.contains("active_order")) {
            out["active_order"] = nullptr;
        }
        return out;
    }

    json parents_snapshot() {
        json out = json::array();
        for (auto& id : parents) {
            try {
                out.push_back(normalise_parent(engine->get_parent(id)));
            } catch (const std::exception& e) {
                out.push_back(json{{"error", e.what()}});
            }
        }
        return out;
    }

    // Replaces "$order_id:k" / "$order_qty:k" / "$order_px:k" tokens in feedback payloads.
    json substitute(const json& value) {
        if (value.is_string()) {
            const std::string& s = value.get_ref<const std::string&>();
            for (const char* prefix : {"$order_id:", "$order_qty:", "$order_px:", "$order_part:"}) {
                std::string p = prefix;
                if (s.rfind(p, 0) == 0) {
                    std::string rest = s.substr(p.size());
                    std::string fraction = "0.5";
                    if (auto colon = rest.find(':'); colon != std::string::npos) {
                        fraction = rest.substr(colon + 1);
                        rest = rest.substr(0, colon);
                    }
                    long long k = std::stoll(rest);
                    auto id = order_id(json(k));
                    if (!id) return json("unknown-order-" + std::to_string(k));
                    if (p == "$order_id:") return json(*id);
                    auto it = submit_by_id.find(*id);
                    if (it == submit_by_id.end()) return json(nullptr);
                    if (p == "$order_part:") {
                        const json& q = it->second.at("quantity");
                        ng::Decimal quantity = q.is_string() ? ng::Decimal::parse(q.get<std::string>()) : ng::Decimal::from_double(q.get<double>());
                        ng::Decimal part = quantity * ng::Decimal::parse(fraction);
                        return json(std::to_string(static_cast<long long>(part.to_double())));
                    }
                    const json& field = p == "$order_qty:" ? it->second.at("quantity") : it->second.at("limit_price");
                    if (field.is_string()) return json(std::stod(field.get<std::string>()));
                    return field;
                }
            }
            return value;
        }
        if (value.is_object()) {
            json out = json::object();
            for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = substitute(it.value());
            return out;
        }
        if (value.is_array()) {
            json out = json::array();
            for (auto& v : value) out.push_back(substitute(v));
            return out;
        }
        return value;
    }
};

json get_or(const json& step, const char* key, json fallback) {
    return step.contains(key) ? step.at(key) : fallback;
}

json run_smart_step(Session& s, const json& step) {
    const std::string op = step.value("op", "");
    json result = json::object();
    if (op == "now") {
        s.now = step.at("t").get<double>();
        result["now"] = s.now;
    } else if (op == "start") {
        json parent = s.engine->start_parent(step.at("settings"));
        s.parents.push_back(parent.at("parent_id").get<std::string>());
        result["parent"] = static_cast<int>(s.parents.size()) - 1;
        result["created"] = s.normalise_parent(parent);
    } else if (op == "quote") {
        QuoteUpdate u;
        u.bid = step.at("bid");
        u.ask = step.at("ask");
        u.bid_size = step.at("bid_size");
        json source = get_or(step, "source_time", json("wall"));
        u.source_time = source.is_string() && source == "wall" ? json(s.wall()) : source;
        if (step.contains("sequence") && !step.at("sequence").is_null()) u.sequence = step.at("sequence").get<long long>();
        u.healthy = step.value("healthy", true);
        u.accessible = step.value("accessible", true);
        u.reset_sequence = step.value("reset_sequence", false);
        u.preserve_consumption = step.value("preserve_consumption", false);
        s.engine->on_quote(step.value("symbol", "XYZ"), u);
    } else if (op == "feed_health") {
        s.engine->set_feed_health(step.value("symbol", "XYZ"), step.value("healthy", true), step.value("reason", ""),
                                  step.value("reset_sequence", false));
    } else if (op == "trade") {
        TradeUpdate t;
        t.quantity = step.at("quantity");
        t.event_id = step.at("event_id").get<std::string>();
        json source = get_or(step, "source_time", json("wall"));
        t.source_time = source.is_string() && source == "wall" ? json(s.wall()) : source;
        if (step.contains("sequence") && !step.at("sequence").is_null()) t.sequence = step.at("sequence").get<long long>();
        if (step.contains("correction_of") && !step.at("correction_of").is_null()) t.correction_of = step.at("correction_of").get<std::string>();
        t.healthy = step.value("healthy", true);
        if (step.contains("interval_start")) t.interval_start = step.at("interval_start");
        s.engine->on_trade(step.value("symbol", "XYZ"), t);
    } else if (op == "tick") {
        result["actions"] = s.normalise_actions(s.engine->tick());
    } else if (op == "drain") {
        result["actions"] = s.normalise_actions(s.engine->drain_actions());
    } else if (op == "report") {
        auto id = s.order_id(step.at("order"));
        if (!id) throw std::runtime_error("unknown order index");
        OrderReport r;
        r.status = step.at("status").get<std::string>();
        r.cumulative_filled = s.substitute(step.at("cumulative"));
        if (step.contains("broker_order_id") && !step.at("broker_order_id").is_null()) r.broker_order_id = step.at("broker_order_id").get<std::string>();
        if (step.contains("sequence") && !step.at("sequence").is_null()) r.sequence = step.at("sequence").get<long long>();
        r.authoritative = step.value("authoritative", true);
        r.final = step.value("final", false);
        s.engine->on_order_report(*id, r);
    } else if (op == "action_error") {
        auto id = s.order_id(step.at("order"));
        if (!id) throw std::runtime_error("unknown order index");
        s.engine->on_action_error(*id, step.value("action", "submit"), step.value("error", "error"), step.value("definitive", false));
    } else if (op == "prepare") {
        auto id = s.order_id(step.at("order"));
        if (!id) throw std::runtime_error("unknown order index");
        auto prepared = s.engine->prepare_dispatch(*id);
        result["prepared"] = prepared ? s.normalise_action(*prepared) : json(nullptr);
    } else if (op == "abort") {
        auto id = s.order_id(step.at("order"));
        if (!id) throw std::runtime_error("unknown order index");
        result["aborted"] = s.engine->abort_dispatch(*id, step.value("reason", "abort"), step.value("stop_parent", false));
    } else if (op == "hold") {
        auto id = s.order_id(step.at("order"));
        if (!id) throw std::runtime_error("unknown order index");
        result["held"] = s.engine->hold_order(*id, step.value("reason", "conflict"));
    } else if (op == "stop") {
        int k = step.value("parent", 0);
        if (k < 0 || k >= static_cast<int>(s.parents.size())) throw std::runtime_error("unknown parent index");
        result["stopped"] = s.normalise_parent(s.engine->request_stop(s.parents[static_cast<std::size_t>(k)], std::nullopt, step.value("reason", "manual stop")));
    } else if (op == "forecast") {
        int k = step.value("parent", 0);
        if (k < 0 || k >= static_cast<int>(s.parents.size())) throw std::runtime_error("unknown parent index");
        std::optional<ng::Decimal> quantity;
        if (step.contains("quantity") && !step.at("quantity").is_null()) {
            quantity = step.at("quantity").is_string() ? ng::Decimal::parse(step.at("quantity").get<std::string>())
                                                        : ng::Decimal::from_double(step.at("quantity").get<double>());
        }
        s.engine->set_remaining_volume_forecast(s.parents[static_cast<std::size_t>(k)], quantity);
    } else if (op == "restart") {
        s.close();
        s.open();
    } else if (op == "price_book" || op == "volume") {
        std::optional<std::string> topic;
        std::optional<int> partition;
        std::optional<long long> offset;
        if (step.contains("topic") && !step.at("topic").is_null()) topic = step.at("topic").get<std::string>();
        if (step.contains("partition") && !step.at("partition").is_null()) partition = step.at("partition").get<int>();
        if (step.contains("offset") && !step.at("offset").is_null()) offset = step.at("offset").get<long long>();
        bool accepted = op == "price_book" ? s.market->apply_price_book(step.at("payload"), topic, partition, offset)
                                           : s.market->apply_volume(step.at("payload"), topic, partition, offset);
        result["accepted"] = accepted;
        std::string symbol = step.value("symbol", "XYZ");
        auto status = s.market->market_status(symbol);
        result["market_state"] = status.state;
        auto rate = s.market->volume_rate(symbol);
        result["volume_rate"] = rate ? json(rate->str()) : json(nullptr);
        auto q = s.engine->quotes_for_test().find(symbol);
        if (q != s.engine->quotes_for_test().end()) {
            result["quote_healthy"] = q->second.healthy;
            result["quote_gap"] = q->second.sequence_gap;
        }
    } else if (op == "connected") {
        s.market->set_connected(step.value("connected", true));
    } else if (op == "volume_connected") {
        s.market->set_volume_connected(step.value("connected", true));
    } else if (op == "snapshot") {
        json payload = s.substitute(step.at("payload"));
        result["applied"] = s.feedback->apply_snapshot(ng::smart::parse_smart_selling_snapshot(payload));
    } else if (op == "external") {
        s.feedback->track_external_order(s.substitute(step.at("command")));
    } else if (op == "open_orders") {
        result["open"] = s.feedback->has_open_orders(step.value("account_id", "A"), step.value("symbol", "XYZ"));
    } else if (op == "reconcile") {
        auto id = s.order_id(step.at("order"));
        if (!id) throw std::runtime_error("unknown order index");
        result["reconciled"] = s.feedback->reconcile(*id);
    } else if (op == "status") {
        // parents snapshot is always recorded below
    } else {
        throw std::runtime_error("unknown op " + op);
    }
    return result;
}

json run_smart(const json& scenario) {
    Session s;
    std::string dir = (std::filesystem::temp_directory_path() / "ng-oracle-XXXXXX").string();
    std::vector<char> tmpl(dir.begin(), dir.end());
    tmpl.push_back('\0');
    if (!mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
    s.dir = tmpl.data();
    s.config = scenario.value("config", json::object());
    s.open();
    json trace = json::array();
    int index = 0;
    for (const json& step : scenario.at("steps")) {
        json entry;
        entry["step"] = index++;
        entry["op"] = step.value("op", "");
        try {
            entry["result"] = run_smart_step(s, step);
            entry["error"] = nullptr;
        } catch (const std::exception& e) {
            entry["result"] = json::object();
            entry["error"] = e.what();
        }
        entry["parents"] = s.parents_snapshot();
        trace.push_back(std::move(entry));
    }
    s.close();
    std::error_code ec;
    std::filesystem::remove_all(s.dir, ec);
    return trace;
}

json run_fast_case(const json& c) {
    std::vector<ng::AccountMeta> metas;
    for (auto it = c.at("metas").begin(); it != c.at("metas").end(); ++it) {
        ng::AccountMeta m;
        m.id = it.key();
        m.num_id = it.value().value("num_id", 0);
        m.broker = it.value().value("broker", "Tiger");
        m.trading_medium = it.value().value("trading_medium", "API");
        metas.push_back(m);
    }
    ng::AccountDirectory directory(metas);
    ng::OrderBookSnapshot book;
    book.symbol = c.value("symbol", "XYZ");
    for (auto& level : c.at("book").value("asks", json::array())) book.asks.push_back(ng::BookLevel{level.at(0).get<double>(), level.at(1).get<double>(), std::nullopt});
    for (auto& level : c.at("book").value("bids", json::array())) book.bids.push_back(ng::BookLevel{level.at(0).get<double>(), level.at(1).get<double>(), std::nullopt});
    std::map<std::string, ng::AccountSnapshot> snapshots;
    bool have_snapshots = c.contains("snapshots") && !c.at("snapshots").is_null();
    if (have_snapshots) {
        for (auto it = c.at("snapshots").begin(); it != c.at("snapshots").end(); ++it) {
            ng::AccountSnapshot snap;
            snap.account_id = it.key();
            snap.cash = it.value().value("cash", 0.0);
            if (it.value().contains("cash_by_currency")) for (auto cit = it.value().at("cash_by_currency").begin(); cit != it.value().at("cash_by_currency").end(); ++cit) snap.cash_by_currency[cit.key()] = cit.value().get<double>();
            if (it.value().contains("available_cash_by_currency")) for (auto cit = it.value().at("available_cash_by_currency").begin(); cit != it.value().at("available_cash_by_currency").end(); ++cit) snap.available_cash_by_currency[cit.key()] = cit.value().get<double>();
            for (auto& p : it.value().value("positions", json::array())) {
                ng::Position pos;
                pos.symbol = p.at(0).get<std::string>();
                pos.qty = p.at(1).get<double>();
                if (p.size() > 2 && !p.at(2).is_null()) pos.available_qty = p.at(2).get<double>();
                snap.positions.push_back(pos);
            }
            snap.trading_enabled = it.value().value("trading_enabled", true);
            snapshots[it.key()] = snap;
        }
    }
    std::map<std::string, double> minimum_cycle;
    bool have_minimum = c.contains("minimum_cycle_seconds_by_medium") && !c.at("minimum_cycle_seconds_by_medium").is_null();
    if (have_minimum) for (auto it = c.at("minimum_cycle_seconds_by_medium").begin(); it != c.at("minimum_cycle_seconds_by_medium").end(); ++it) minimum_cycle[it.key()] = it.value().get<double>();
    std::map<std::string, double> last_action;
    bool have_last = c.contains("last_action") && !c.at("last_action").is_null();
    if (have_last) for (auto it = c.at("last_action").begin(); it != c.at("last_action").end(); ++it) last_action[it.key()] = it.value().get<double>();

    ng::fast::CycleInputs in;
    in.trading_mode = c.value("trading_mode", "E");
    in.symbol = c.value("symbol", "XYZ");
    in.book = book;
    in.price_limit = c.value("price_limit", 0.0);
    for (auto& id : c.value("account_ids", json::array())) in.account_ids.push_back(id.get<std::string>());
    in.book_levels = c.value("book_levels", 1);
    in.target_percentage = c.value("target_percentage", 100);
    in.account_metas = &directory;
    in.account_snapshots = have_snapshots ? &snapshots : nullptr;
    in.minimum_cycle_seconds_by_medium = have_minimum ? &minimum_cycle : nullptr;
    in.last_action_monotonic_by_account = have_last ? &last_action : nullptr;
    if (c.contains("now") && !c.at("now").is_null()) in.now_monotonic = c.at("now").get<double>();
    in.cycle_id = "cycle";
    ng::fast::CycleResult r = ng::fast::build_fast_trading_cycle_commands(in);
    json out;
    out["resting_side"] = r.resting_side;
    out["order_side"] = r.order_side;
    out["total_resting_qty"] = r.total_resting_qty;
    out["target_quantity"] = r.target_quantity;
    out["limit_price"] = r.limit_price ? json(*r.limit_price) : json(nullptr);
    out["per_account_qty"] = r.per_account_qty;
    out["allocated_quantity"] = r.allocated_quantity;
    out["cooldown_eligible_account_ids"] = r.cooldown_eligible_account_ids;
    out["capacity_eligible_account_ids"] = r.capacity_eligible_account_ids;
    out["capacity_noop"] = r.capacity_noop;
    out["reason"] = r.reason;
    out["commands"] = json::array();
    for (auto& cmd : r.commands) {
        json n = json::object();
        for (const char* key : {"type", "account_id", "symbol", "side", "qty_shares", "limit_price", "trading_mode", "time_in_force",
                                "fast_trading_resting_side", "fast_trading_total_resting_qty", "fast_trading_target_percentage",
                                "fast_trading_book_levels", "fast_trading_price_limit", "broker", "test_mode"}) {
            if (cmd.contains(key)) n[key] = cmd.at(key);
        }
        out["commands"].push_back(n);
    }
    return out;
}

json run_fast(const json& scenario) {
    json trace = json::array();
    int index = 0;
    for (const json& c : scenario.at("cases")) {
        json entry;
        entry["case"] = index++;
        try {
            entry["result"] = run_fast_case(c);
            entry["error"] = nullptr;
        } catch (const std::exception& e) {
            entry["result"] = nullptr;
            entry["error"] = e.what();
        }
        trace.push_back(std::move(entry));
    }
    return trace;
}

}  // namespace

int main() {
    std::stringstream buffer;
    buffer << std::cin.rdbuf();
    json scenario = json::parse(buffer.str());
    std::string kind = scenario.value("kind", "smart");
    json trace = kind == "fast" ? run_fast(scenario) : run_smart(scenario);
    std::cout << trace.dump() << std::endl;
    return 0;
}
