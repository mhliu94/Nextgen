// Five-minute window accounting on temporary databases (tests/test_market_activity.py).
#include <doctest.h>

#include "core/sqlite.h"
#include "core/time_util.h"
#include "kafka/feeds.h"
#include "model/market_data.h"
#include "orders/order_status.h"
#include "reporting/market_activity.h"
#include "test_helpers.h"

using ng::json;
using ng::StockQuote;
using ng::reporting::MarketActivity;

namespace {

// 2026-11-09T15:20:00Z, the start of a five-minute window.
constexpr double START = 1794237600.0;

ng::AccountDirectory accounts() { return ngtest::directory({ngtest::meta("A", 1, "Tiger"), ngtest::meta("B", 2, "Fangde")}); }

StockQuote quote(double at, std::optional<long long> volume, double price = 10, const std::string& symbol = "XYZ",
                 std::optional<std::string> error = std::nullopt) {
    StockQuote q;
    q.symbol = symbol;
    q.last = price;
    q.trade_time = at - 1;
    q.observed_at = at;
    q.is_delayed = false;
    q.volume = volume;
    q.last_size = 25;
    q.error = std::move(error);
    return q;
}

struct OrderSpec {
    long long sequence = 1;
    std::string order_id = "one";
    std::string side = "BUY";
    std::string account = "A";
    double created = START;
    std::string symbol = "XYZ";
};

ng::orders::OrderEvent order(double at, json filled, OrderSpec spec = {}) {
    bool any = filled.is_number() && filled.get<double>() != 0;
    json payload = {{"schema_version", 1},
                    {"account_id", spec.account},
                    {"order_id", spec.order_id},
                    {"sequence", spec.sequence},
                    {"created_at", ng::iso_utc_offset(spec.created)},
                    {"updated_at", ng::iso_utc_offset(at)},
                    {"symbol", spec.symbol},
                    {"side", spec.side},
                    {"order_type", "LIMIT"},
                    {"status", any ? "PARTIALLY_FILLED" : "NEW"},
                    {"quantity", 1000},
                    {"filled_quantity", filled},
                    {"average_fill_price", any ? json(10) : json(nullptr)},
                    {"limit_price", 10},
                    {"currency", "USD"}};
    return ng::orders::parse_order_message(payload.dump());
}

struct History {
    ngtest::TempDir dir{"market-activity"};
    double now = START + 120;
    std::unique_ptr<MarketActivity> db;

    History() { reopen(); }
    void reopen() { db = std::make_unique<MarketActivity>(dir.file("market.sqlite3"), [this] { return now; }); }
    json snapshot(std::optional<double> at = std::nullopt, const std::string& feed = "live", const std::string& symbol = "XYZ") {
        return db->snapshot(symbol, accounts(), feed, at ? *at : START + 120);
    }
};

}  // namespace

TEST_CASE_FIXTURE(History, "four aligned windows and current rollover") {
    json result = snapshot();
    std::vector<double> starts;
    for (const auto& row : result["windows"]) {
        starts.push_back(row["start"].get<double>());
        CHECK(row["end"].get<double>() - row["start"].get<double>() == 300);
    }
    CHECK(starts == std::vector<double>{START, START - 300, START - 600, START - 900});
    CHECK(result["windows"][0]["current"] == true);
    for (int i = 1; i < 4; ++i) CHECK(result["windows"][i]["current"] == false);
    CHECK(snapshot(START + 300)["windows"][0]["start"] == START + 300);
    CHECK(snapshot(START - .001)["windows"][0]["start"] == START - 300);
    CHECK(result["timezone"] == "America/New_York");
    CHECK(MarketActivity::window_start(START + 299.999) == static_cast<long long>(START));
}

TEST_CASE_FIXTURE(History, "counter deltas weight prices and do not use last trade size") {
    struct Sample { double at; long long volume; double price; };
    for (Sample s : {Sample{0, 1000, 99}, Sample{10, 1100, 10}, Sample{20, 1400, 20}, Sample{30, 1400, 999}}) {
        db->record_quote(quote(START + s.at, s.volume, s.price));
    }
    json result = snapshot(START + 30);
    const json& row = result["windows"][0];
    CHECK(row["market_volume"] == 400);
    CHECK(row["vwap_estimate"] == 17.5);
    CHECK(row["market_complete"] == true);
    CHECK(result["last_trade"] == json{{"time", START + 29}, {"quantity", 25}, {"price", 999.0}});
    CHECK(result["quote_state"] == "live");
}

TEST_CASE_FIXTURE(History, "no history and no trades are distinct") {
    json row = snapshot()["windows"][0];
    CHECK(row["market_volume"].is_null());
    CHECK(row["vwap_estimate"].is_null());
    CHECK(row["market_complete"] == false);
    db->record_quote(quote(START, 500));
    db->record_quote(quote(START + 20, 500));
    row = snapshot(START + 20)["windows"][0];
    CHECK(row["market_volume"] == 0);
    CHECK(row["vwap_estimate"].is_null());
    CHECK(row["market_complete"] == true);
}

TEST_CASE_FIXTURE(History, "market volume at a boundary is not double counted") {
    struct Sample { double at; long long volume; };
    for (Sample s : {Sample{-20, 100}, Sample{0, 120}, Sample{20, 150}}) db->record_quote(quote(START + s.at, s.volume));
    json rows = snapshot(START + 20)["windows"];
    CHECK(rows[0]["market_volume"] == 30);
    CHECK(rows[1]["market_volume"] == 20);
    CHECK(rows[1]["market_complete"] == false);
}

TEST_CASE_FIXTURE(History, "duplicates and out of order quotes are idempotent across restart") {
    for (double at : {20.0, 0.0, 10.0, 20.0}) db->record_quote(quote(START + at, 100 + static_cast<long long>(at)));
    json before = snapshot(START + 20);
    db->close();
    reopen();
    CHECK(snapshot(START + 20) == before);
    CHECK(before["windows"][0]["market_volume"] == 20);
}

TEST_CASE("counter resets and bad quotes do not invent volume") {
    for (bool reset : {true, false}) {
        CAPTURE(reset);
        History h;
        StockQuote middle = reset ? quote(START + 10, 900) : quote(START + 10, std::nullopt, 10, "XYZ", std::string("unavailable"));
        h.db->record_quote(quote(START, 1000));
        h.db->record_quote(middle);
        h.db->record_quote(quote(START + 20, 1000));
        json row = h.snapshot(START + 20)["windows"][0];
        CHECK(row["market_volume"] == (reset ? 100 : 0));
        CHECK(row["market_complete"] == false);
    }
}

TEST_CASE_FIXTURE(History, "stale feed and large gaps are explicit") {
    db->record_quote(quote(START, 1000));
    db->record_quote(quote(START + 120, 5000));
    json row = snapshot()["windows"][0];
    CHECK(row["market_volume"] == 0);
    CHECK(row["vwap_estimate"].is_null());
    CHECK(row["market_complete"] == false);
    now = START + 151;
    json stale = snapshot(START + 151);
    CHECK(stale["quote_state"] == "unavailable");
    for (const auto& value : stale["last_trade"]) CHECK(value.is_null());
}

TEST_CASE_FIXTURE(History, "daily counter reset has no negative volume") {
    // 2026-11-10T05:00:00Z is midnight in New York.
    const double midnight = 1794286800.0;
    now = midnight + 20;
    struct Sample { double at; long long volume; };
    for (Sample s : {Sample{-10, 50000}, Sample{0, 0}, Sample{10, 100}}) db->record_quote(quote(midnight + s.at, s.volume));
    json rows = snapshot(midnight + 10)["windows"];
    CHECK(rows[0]["market_volume"] == 100);
    CHECK(rows[1]["market_volume"].is_null());
    CHECK(rows[1]["market_complete"] == false);
}

TEST_CASE_FIXTURE(History, "own fills are deltas by report time with buy sell gross and account filter") {
    now = START + 320;
    std::vector<ng::orders::OrderEvent> events = {
        order(START, 0),
        order(START + 20, 10, {.sequence = 2}),
        order(START + 30, 10, {.sequence = 3}),
        order(START + 301, 25, {.sequence = 4}),
        order(START + 302, 5.5, {.order_id = "two", .side = "SELL", .account = "B", .created = START + 300}),
        order(START + 310, 900, {.order_id = "hidden", .account = "HIDDEN", .created = START + 300}),
    };
    for (const auto& event : events) db->record_order(event);
    db->record_order(events[1]);
    db->record_order(events[3]);
    json rows = snapshot(START + 320)["windows"];
    CHECK(rows[0]["bought"] == 15.0);
    CHECK(rows[0]["sold"] == 5.5);
    CHECK(rows[0]["gross"] == 20.5);
    CHECK(rows[1]["bought"] == 10.0);
    CHECK(rows[1]["sold"] == 0.0);
    CHECK(rows[1]["gross"] == 10.0);
    CHECK(rows[0]["our_complete"] == false);
}

TEST_CASE_FIXTURE(History, "fill replay order does not change totals or move fills on status only updates") {
    now = START + 320;
    std::vector<ng::orders::OrderEvent> events = {order(START, 0), order(START + 10, 10, {.sequence = 2}),
                                                  order(START + 310, 20, {.sequence = 3}), order(START + 320, 20, {.sequence = 4})};
    for (auto it = events.rbegin(); it != events.rend(); ++it) db->record_order(*it);
    json rows = snapshot(START + 320)["windows"];
    CHECK(rows[0]["bought"] == 10.0);
    CHECK(rows[1]["bought"] == 10.0);
    CHECK(db->record_order(events.back()) == false);
}

TEST_CASE_FIXTURE(History, "cumulative corrections remove prior fills without negative gross") {
    now = START + 320;
    for (const auto& event : {order(START, 0), order(START + 10, 10, {.sequence = 2}), order(START + 310, 30, {.sequence = 3}),
                              order(START + 320, 5, {.sequence = 4})}) {
        db->record_order(event);
    }
    json rows = snapshot(START + 320)["windows"];
    CHECK(rows[0]["gross"] == 0.0);
    CHECK(rows[1]["gross"] == 5.0);
    CHECK(rows[0]["our_complete"] == false);
    CHECK(rows[1]["our_complete"] == false);
}

TEST_CASE_FIXTURE(History, "missing order feed never reports an all clear zero") {
    json result = snapshot(std::nullopt, "replaying");
    for (const auto& row : result["windows"]) {
        CHECK(row["gross"].is_null());
        CHECK(row["bought"].is_null());
        CHECK(row["sold"].is_null());
        CHECK(row["our_complete"] == false);
    }
    db->record_order(order(START + 10, 5));
    result = snapshot(std::nullopt, "disconnected");
    CHECK(result["windows"][0]["gross"] == 5.0);
    CHECK(result["windows"][0]["our_complete"] == false);
}

TEST_CASE_FIXTURE(History, "late fill on prior day order and prefixed symbols are included") {
    db->record_order(order(START - 86400, 10, {.created = START - 86400, .symbol = "US.XYZ"}));
    db->record_order(order(START + 10, 20, {.sequence = 2, .created = START - 86400, .symbol = "US.XYZ"}));
    CHECK(snapshot()["windows"][0]["bought"] == 10.0);
}

TEST_CASE_FIXTURE(History, "conflicting sequences and identity changes are rejected") {
    db->record_order(order(START + 20, 10, {.sequence = 2}));
    CHECK_THROWS_WITH_AS(db->record_order(order(START + 20, 20, {.sequence = 2})), "Conflicting order volume snapshot",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(db->record_order(order(START + 10, 20, {.sequence = 3})), "Order report time decreased",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(db->record_order(order(START + 30, 20, {.sequence = 3, .side = "SELL"})), "Order identity changed",
                         std::invalid_argument);
}

TEST_CASE_FIXTURE(History, "obsolete retained reports cannot poison the current own-volume window") {
    const double created=START-86400;
    REQUIRE(db->record_order(order(created+30,20,{.sequence=2,.created=created})));
    CHECK_FALSE(db->record_order(order(created+10,5,{.sequence=0,.created=created})));
    CHECK_FALSE(db->record_order(order(created+20,10,{.sequence=0,.created=created})));
    CHECK_THROWS(db->record_order(order(created+20,10,{.sequence=0,.created=created,.symbol="OTHER"})));
    REQUIRE(db->record_order(order(START+10,25,{.sequence=3,.created=created})));
}

TEST_CASE_FIXTURE(History, "equivalent numeric fills are deduplicated") {
    CHECK(db->record_order(order(START + 10, 10)));
    CHECK(db->record_order(order(START + 10, 10.0)) == false);
    CHECK(snapshot()["windows"][0]["gross"] == 10.0);
}

TEST_CASE_FIXTURE(History, "hidden account history does not mask unavailable own volume") {
    db->record_order(order(START + 10, 10, {.account = "HIDDEN"}));
    CHECK(snapshot(std::nullopt, "disconnected")["windows"][0]["gross"].is_null());
}

TEST_CASE_FIXTURE(History, "retention preserves an old order baseline") {
    const double created = START - 3 * 86400;
    db->record_order(order(created, 10, {.created = created}));
    db->record_order(order(created + 30, 20, {.sequence = 2, .created = created}));
    db->record_quote(quote(START, 100));  // Runs cleanup.
    {
        ng::Sqlite raw(dir.file("market.sqlite3"), ng::Sqlite::Mode::ReadOnly);
        ng::Statement count(raw, "SELECT COUNT(*) FROM market_order_samples");
        REQUIRE(count.step());
        CHECK(count.column_int(0) == 1);
    }
    db->record_order(order(START + 10, 25, {.sequence = 3, .created = created}));
    CHECK(snapshot()["windows"][0]["bought"] == 5.0);
}

TEST_CASE_FIXTURE(History, "quotes outside the retained day or from the future are ignored") {
    db->record_quote(quote(now - 86401, 1));
    db->record_quote(quote(now + 1.5, 2));
    db->record_quote(quote(now + 1, 3));
    ng::Sqlite raw(dir.file("market.sqlite3"), ng::Sqlite::Mode::ReadOnly);
    ng::Statement count(raw, "SELECT COUNT(*), MAX(volume) FROM market_quote_samples");
    REQUIRE(count.step());
    CHECK(count.column_int(0) == 1);
    CHECK(count.column_int(1) == 3);
}

TEST_CASE_FIXTURE(History, "consumer persists quotes and a missing size does not invalidate the price") {
    json payload = {{"type", "STOCK_QUOTE"}, {"schema_version", 1}, {"symbol", "XYZ"}, {"healthy", true}, {"is_delayed", false},
                    {"observed_at", ng::iso_utc_offset(START + 120)}, {"trade_time", ng::iso_utc_offset(START + 119)},
                    {"last", "10"}, {"volume", 100}, {"last_size", 42}};
    auto parsed = ng::stock_quote_from_payload(payload);
    REQUIRE(parsed);
    db->record_quote(*parsed);
    CHECK(snapshot()["last_trade"]["quantity"] == 42);
    for (const json& missing : {json(nullptr), json(-1), json(true), json("bad")}) {
        json changed = payload;
        changed["last_size"] = missing;
        auto quote_without_size = ng::stock_quote_from_payload(changed);
        REQUIRE(quote_without_size);
        CHECK_FALSE(quote_without_size->last_size);
        CHECK(quote_without_size->last == 10.0);
    }
}
