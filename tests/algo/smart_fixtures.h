// Shared fixtures mirroring the Python Smart selling test harnesses.
#pragma once

#include <doctest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/decimal.h"
#include "core/sqlite.h"
#include "core/time_util.h"
#include "algo/smart_selling/engine.h"
#include "test_helpers.h"

namespace doctest {
template <>
struct StringMaker<ng::Decimal> {
    static String convert(const ng::Decimal& value) { return value.str().c_str(); }
};
}  // namespace doctest

namespace ngtest {

using ng::Decimal;
using ng::json;
using ng::smart::OrderReport;
using ng::smart::QuoteUpdate;
using ng::smart::SmartSellingEngine;
using ng::smart::TradeUpdate;

inline Decimal Dec(const json& value) {
    if (value.is_string()) return Decimal::parse(value.get<std::string>());
    if (value.is_number_integer()) return Decimal(value.get<long long>());
    return Decimal::from_double(value.get<double>());
}
inline std::string S(const json& value) { return value.get<std::string>(); }
inline Decimal Dec(const char* text) { return Decimal::parse(text); }
inline Decimal Dec(int value) { return Decimal(value); }

inline std::vector<std::string> action_types(const std::vector<json>& actions) {
    std::vector<std::string> out;
    for (auto& a : actions) out.push_back(a.at("type").get<std::string>());
    return out;
}

struct EngineFixture {
    TempDir dir{"smart-engine"};
    double now = 0.0;
    double wall_start = 1800000000.0;
    std::string path;
    std::unique_ptr<SmartSellingEngine> engine;
    std::string parent;

    EngineFixture() : path(dir.file("smart.sqlite3")) { reopen(); }
    double wall() const { return wall_start + now; }
    void reopen() {
        engine = std::make_unique<SmartSellingEngine>(path, [this] { return now; }, [this] { return wall(); });
    }

    json start(json overrides = json::object()) {
        json settings = {{"account_id", "A"}, {"symbol", "XYZ"},      {"quantity", "1000"},   {"mode", "urgent"},
                         {"horizon_seconds", 300}, {"tick_size", ".01"}, {"quantity_step", "1"}, {"min_quantity", "1"},
                         {"min_notional", "0"}};
        for (auto it = overrides.begin(); it != overrides.end(); ++it) settings[it.key()] = it.value();
        json p = engine->start_parent(settings);
        parent = p.at("parent_id").get<std::string>();
        quote();
        return p;
    }

    void quote(json overrides = json::object()) {
        QuoteUpdate u;
        u.bid = overrides.value("bid", json("10"));
        u.ask = overrides.value("ask", json("10.01"));
        u.bid_size = overrides.value("bid_size", json("1000"));
        u.source_time = overrides.contains("source_time") ? overrides.at("source_time") : json(wall());
        if (overrides.contains("sequence")) u.sequence = overrides.at("sequence").get<long long>();
        if (overrides.contains("reset_sequence")) u.reset_sequence = overrides.at("reset_sequence").get<bool>();
        if (overrides.contains("preserve_consumption")) u.preserve_consumption = overrides.at("preserve_consumption").get<bool>();
        if (overrides.contains("healthy")) u.healthy = overrides.at("healthy").get<bool>();
        if (overrides.contains("ask_size")) u.ask_size = overrides.at("ask_size");
        if (overrides.contains("bid_prices")) u.bid_prices = overrides.at("bid_prices").get<std::vector<json>>();
        if (overrides.contains("ask_prices")) u.ask_prices = overrides.at("ask_prices").get<std::vector<json>>();
        engine->on_quote("XYZ", u);
    }

    // Rewrites the persisted parent document directly (bypassing the ledger checks), like the Python
    // tests that patch a stored row to model legacy or externally modified journals.
    void patch_stored_parent(const std::function<void(json&)>& change) {
        ng::Sqlite db(path);
        ng::Statement select(db, "SELECT data FROM smart_parents WHERE id=?");
        select.bind(1, parent);
        REQUIRE(select.step());
        json stored = json::parse(select.column_text(0));
        change(stored);
        ng::Statement update(db, "UPDATE smart_parents SET data=? WHERE id=?");
        update.bind(1, stored.dump());
        update.bind(2, parent);
        update.exec();
    }

    void tape(const std::string& quantity = "1000", const std::string& event_id = "trade-1", json extra = json::object()) {
        TradeUpdate t;
        t.quantity = quantity;
        t.event_id = event_id;
        t.source_time = extra.contains("source_time") ? extra.at("source_time") : json(wall());
        if (extra.contains("sequence")) t.sequence = extra.at("sequence").get<long long>();
        if (extra.contains("correction_of")) t.correction_of = extra.at("correction_of").get<std::string>();
        if (extra.contains("interval_start")) t.interval_start = extra.at("interval_start");
        if (extra.contains("healthy")) t.healthy = extra.at("healthy").get<bool>();
        engine->on_trade("XYZ", t);
    }

    void correction(const std::string& quantity, const std::string& event_id, const std::string& of) {
        TradeUpdate t;
        t.quantity = quantity;
        t.event_id = event_id;
        t.correction_of = of;
        engine->on_trade("XYZ", t);
    }

    json child() {
        std::vector<json> actions = engine->tick();
        std::vector<json> submits;
        for (auto& a : actions) if (a.at("type") == "submit") submits.push_back(a);
        REQUIRE(submits.size() == 1);
        return submits[0];
    }

    void report(const json& child, const std::string& status, const std::string& quantity = "0", json extra = json::object()) {
        OrderReport r;
        r.status = status;
        r.cumulative_filled = quantity;
        r.broker_order_id = "broker-" + child.at("client_order_id").get<std::string>();
        if (extra.contains("sequence")) r.sequence = extra.at("sequence").get<long long>();
        if (extra.contains("final")) r.final = extra.at("final").get<bool>();
        if (extra.contains("authoritative")) r.authoritative = extra.at("authoritative").get<bool>();
        engine->on_order_report(child.at("client_order_id").get<std::string>(), r);
    }

    json status() {
        json result = engine->get_parent(parent);
        Decimal sum = Dec(result["free_credit"]) + Dec(result["reserved"]) + Dec(result["filled"]) + Dec(result["discarded"]);
        CHECK(Dec(result["earned"]) == sum);
        CHECK(Dec(result["filled"]) + Dec(result["reserved"]) <= Dec(result["quantity"]));
        Decimal f, r;
        for (auto it = result["accounts"].begin(); it != result["accounts"].end(); ++it) {
            f += Dec(it.value()["filled"]);
            r += Dec(it.value()["reserved"]);
            CHECK(Dec(it.value()["reserved"]) >= Decimal::zero());
            CHECK(Dec(it.value()["filled"]) + Dec(it.value()["reserved"]) <= Dec(it.value()["quantity"]));
        }
        CHECK(f == Dec(result["filled"]));
        CHECK(r == Dec(result["reserved"]));
        return result;
    }
};

}  // namespace ngtest
