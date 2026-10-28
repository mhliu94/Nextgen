#include "smart_fixtures.h"
#include "algo/smart_selling/market_adapter.h"

using namespace ngtest;
using ng::smart::SmartSellingMarketAdapter;

namespace {

struct MarketFixture {
    TempDir dir{"smart-market"};
    double now = 0.0;
    double start_wall = 1800000000.0;
    std::unique_ptr<SmartSellingEngine> engine;
    std::unique_ptr<SmartSellingMarketAdapter> market;
    std::string parent;
    long long book_sequence = 0;

    MarketFixture() {
        engine = std::make_unique<SmartSellingEngine>(dir.file("smart.sqlite3"), [this] { return now; }, [this] { return wall(); });
        market = fresh_adapter();
        parent = start();
    }
    double wall() const { return start_wall + now; }
    std::string iso(double age = 0) const { return ng::iso_utc(wall() - age); }
    std::unique_ptr<SmartSellingMarketAdapter> fresh_adapter(int max_observations = 256) {
        return std::make_unique<SmartSellingMarketAdapter>(*engine, [this] { return now; }, [this] { return wall(); }, 10, 300, max_observations);
    }
    std::string start(json overrides = json::object()) {
        json settings = {{"account_id", "A"}, {"symbol", "XYZ"}, {"quantity", "1000"}, {"mode", "urgent"}, {"horizon_seconds", 300},
                         {"tick_size", ".01"}, {"quantity_step", "1"}, {"min_quantity", "1"}, {"min_notional", "0"}};
        for (auto it = overrides.begin(); it != overrides.end(); ++it) settings[it.key()] = it.value();
        return engine->start_parent(settings)["parent_id"].get<std::string>();
    }
    json book(json overrides = json::object()) {
        ++book_sequence;
        json payload = {{"symbol", "XYZ"},
                        {"update_time", static_cast<long long>(wall() * 1e9)},
                        {"sequence_num", book_sequence},
                        {"bid_side", {{"levels", json::array({{{"price", "10"}, {"quantity", "1000"}}})}}},
                        {"offer_side", {{"levels", json::array({{{"price", "10.02"}, {"quantity", "1000"}}})}}}};
        for (auto it = overrides.begin(); it != overrides.end(); ++it) payload[it.key()] = it.value();
        return payload;
    }
    json volume(long long sequence, json total, json overrides = json::object()) {
        json payload = {{"schema_version", 1}, {"type", "TRADING_VOLUME"}, {"symbol", "XYZ"}, {"source", "TradeStation"},
                        {"feed_id", "feed-a"}, {"sequence", sequence}, {"total_volume", total}, {"observed_at", iso()},
                        {"interval_start", iso(1)}, {"session", "2027-01-15"}, {"healthy", true}, {"is_halted", false},
                        {"is_delayed", false}};
        for (auto it = overrides.begin(); it != overrides.end(); ++it) payload[it.key()] = it.value();
        return payload;
    }
    Decimal earned(const std::string& id = "") { return Dec(engine->get_parent(id.empty() ? parent : id)["earned"]); }
    const ng::smart::QuoteState& quote() { return engine->quotes_for_test().at("XYZ"); }
    std::string reason(const std::string& id = "") { return engine->get_parent(id.empty() ? parent : id)["reason"].get<std::string>(); }
};

}  // namespace

TEST_CASE_FIXTURE(MarketFixture, "price book volume and embedded trades never earn credit") {
    market->apply_price_book(book({{"volume", 1000000}, {"trades", json::array({{{"event_id", "wrong-topic"}, {"quantity", 1000}, {"source_time", wall()}}})}}));
    now = 1;
    market->apply_price_book(book({{"volume", 2000000}}));
    CHECK(earned() == Dec(0));
    CHECK_FALSE(market->volume_rate("XYZ"));
}

TEST_CASE_FIXTURE(MarketFixture, "normalizes best prices and sums same price depth exactly") {
    market->apply_volume(volume(1, 1000));
    market->apply_price_book(book({{"bid_side", {{"levels", json::array({{{"prc", "9.99"}, {"qty", "9000"}}, {{"Price", "10.10"}, {"TotalSize", "100.1"}}, {{"price", "10.10"}, {"total_qty", "200.2"}}})}}},
                                   {"offer_side", {{"levels", json::array({{{"price", "10.30"}, {"qty", 20}}, {{"prc", "10.20"}, {"qty", 10}}})}}}}));
    CHECK(quote().bid == Dec("10.10"));
    CHECK(quote().ask == Dec("10.20"));
    CHECK(quote().bid_size == Dec("300.3"));
    CHECK(quote().healthy);
}

TEST_CASE_FIXTURE(MarketFixture, "first snapshot baseline and exact deltas with duplicate rejection") {
    market->apply_volume(volume(1, "12345.1"));
    CHECK(earned() == Dec(0));
    now = 1;
    json batch = volume(2, "12445.6");
    CHECK(market->apply_volume(batch));
    CHECK(earned() == Dec("10.05"));
    CHECK_FALSE(market->apply_volume(batch));
    now = 2;
    CHECK_FALSE(market->apply_volume(volume(1, "90000")));
    CHECK(earned() == Dec("10.05"));
}

TEST_CASE_FIXTURE(MarketFixture, "straddling batch is excluded only for new parent") {
    market->apply_volume(volume(1, 1000));
    now = 0.5;
    std::string new_parent = engine->start_parent(json{{"account_id", "B"}, {"symbol", "XYZ"}, {"quantity", 1000}, {"mode", "urgent"},
                                                       {"tick_size", ".01"}, {"quantity_step", 1}, {"min_quantity", 1}})["parent_id"].get<std::string>();
    now = 1;
    market->apply_volume(volume(2, 1100));
    CHECK(earned() == Dec(10));
    CHECK(earned(new_parent) == Dec(0));
    now = 2;
    market->apply_volume(volume(3, 1200));
    CHECK(earned() == Dec(20));
    CHECK(earned(new_parent) == Dec(10));
}

TEST_CASE_FIXTURE(MarketFixture, "sequence gap discards uncertain volume and resumes after baseline") {
    market->apply_volume(volume(1, 1000));
    now = 1;
    market->apply_volume(volume(3, 100000));
    CHECK(earned() == Dec(0));
    now = 2;
    market->apply_volume(volume(4, 100100));
    CHECK(earned() == Dec(10));
}

TEST_CASE_FIXTURE(MarketFixture, "stale future and long intervals never create catch-up credit") {
    market->apply_volume(volume(1, 1000));
    now = 20;
    CHECK_FALSE(market->apply_volume(volume(2, 9999, {{"observed_at", iso(15)}})));
    market->apply_volume(volume(3, 10000));
    CHECK(earned() == Dec(0));
    now = 21;
    CHECK_FALSE(market->apply_volume(volume(4, 99999, {{"observed_at", iso(-10)}})));
    now = 40;
    market->apply_volume(volume(5, 100000));
    CHECK(earned() == Dec(0));
    now = 41;
    market->apply_volume(volume(6, 100100));
    CHECK(earned() == Dec(10));
    now = 60;
    market->apply_volume(volume(7, 999999));
    CHECK(earned() == Dec(10));
}

TEST_CASE_FIXTURE(MarketFixture, "volume disconnect blocks saved credit despite fresh quote") {
    market->apply_price_book(book());
    market->apply_volume(volume(1, 1000));
    now = 1;
    market->apply_volume(volume(2, 2000));
    CHECK(earned() == Dec(100));
    market->set_volume_connected(false);
    now = 2;
    market->apply_price_book(book());
    CHECK(engine->tick().empty());
    CHECK(engine->get_parent(parent)["state"] == "HOLD");
    market->set_volume_connected(true);
    market->apply_volume(volume(10, 20000));
    CHECK(earned() == Dec(100));
    now = 3;
    market->apply_volume(volume(11, 20100));
    CHECK(earned() == Dec(110));
}

TEST_CASE_FIXTURE(MarketFixture, "unhealthy observation and session or feed changes reset baseline") {
    market->apply_volume(volume(1, 1000));
    now = 1;
    CHECK_FALSE(market->apply_volume(volume(2, nullptr, {{"healthy", false}})));
    now = 2;
    market->apply_volume(volume(3, 100000));
    now = 3;
    market->apply_volume(volume(4, 100100));
    CHECK(earned() == Dec(10));
    now = 4;
    market->apply_volume(volume(5, 10, {{"session", "2027-01-16"}}));
    now = 5;
    market->apply_volume(volume(1, 20, {{"session", "2027-01-16"}, {"feed_id", "feed-b"}}));
    CHECK(earned() == Dec(10));
    CHECK(reason().find("correction") == std::string::npos);
}

TEST_CASE_FIXTURE(MarketFixture, "unknown negative correction remains held after quotes recover") {
    market->apply_price_book(book());
    market->apply_volume(volume(1, 1000));
    now = 1;
    market->apply_volume(volume(2, 1100));
    now = 2;
    market->apply_volume(volume(3, 1000));
    now = 3;
    market->apply_price_book(book());
    market->apply_volume(volume(4, 1200));
    CHECK(engine->tick().empty());
    json p = engine->get_parent(parent);
    CHECK(p["state"] == "HOLD");
    CHECK(p["reason"].get<std::string>().find("unknown tape correction") != std::string::npos);
}

TEST_CASE_FIXTURE(MarketFixture, "cumulative decrease across sequence gap still requires reconciliation") {
    market->apply_volume(volume(1, 1000));
    now = 1;
    market->apply_volume(volume(2, 1100));
    now = 2;
    market->apply_volume(volume(4, 1050));
    CHECK(reason().find("unknown tape correction") != std::string::npos);
}

TEST_CASE_FIXTURE(MarketFixture, "genuine trade ids and corrections are used without cumulative double credit") {
    market->apply_volume(volume(1, 1000000, {{"trades", json::array({{{"event_id", "trade-1"}, {"quantity", "100.5"}, {"source_time", iso()}}})}}));
    CHECK(earned() == Dec("10.05"));
    now = 1;
    market->apply_volume(volume(2, 2000000, {{"trades", json::array({{{"event_id", "trade-1"}, {"quantity", "100.5"}, {"source_time", iso()}}})}}));
    CHECK(earned() == Dec("10.05"));
    now = 2;
    market->apply_volume(volume(3, 2000000, {{"trades", json::array({{{"event_id", "correct-1"}, {"quantity", "-20.5"}, {"correction_of", "trade-1"}, {"source_time", iso(30)}}})}}));
    CHECK(earned() == Dec("8.00"));
}

TEST_CASE_FIXTURE(MarketFixture, "raw book sequence gaps do not invent provider gaps") {
    market->apply_volume(volume(1, 1000));
    market->apply_price_book(book({{"sequence_num", 1}}));
    now = 1;
    market->apply_price_book(book({{"sequence_num", 50}}));
    CHECK(quote().healthy);
    CHECK_FALSE(quote().sequence_gap);
}

TEST_CASE_FIXTURE(MarketFixture, "explicit book gap holds then recovers on next full snapshot") {
    market->apply_volume(volume(1, 1000));
    market->apply_price_book(book({{"book_sequence", 1}}));
    now = 1;
    market->apply_price_book(book({{"book_sequence", 3}}));
    CHECK_FALSE(quote().healthy);
    now = 2;
    market->apply_price_book(book({{"book_sequence", 4}}));
    CHECK(quote().healthy);
    CHECK_FALSE(quote().sequence_gap);
}

TEST_CASE_FIXTURE(MarketFixture, "quote recovery cannot clear independent trade sequence gap") {
    market->apply_price_book(book());
    market->apply_volume(volume(1, nullptr, {{"trades", json::array({{{"event_id", "trade-1"}, {"quantity", 100}, {"source_time", iso()}, {"sequence", 1}}})}}));
    now = 1;
    market->apply_volume(volume(2, nullptr, {{"trades", json::array({{{"event_id", "trade-3"}, {"quantity", 100}, {"source_time", iso()}, {"sequence", 3}}})}}));
    now = 2;
    market->apply_price_book(book({{"feed_id", "new-book-feed"}}));
    CHECK(engine->tick().empty());
    CHECK(reason().find("sequence gap") != std::string::npos);
}

TEST_CASE_FIXTURE(MarketFixture, "missing or crossed depth holds and stale tape blocks fresh books") {
    market->apply_volume(volume(1, 1000));
    market->apply_price_book(book({{"bid_side", {{"levels", json::array({{{"price", 10}}})}}}}));
    CHECK_FALSE(quote().healthy);
    now = 1;
    market->apply_price_book(book({{"offer_side", {{"levels", json::array({{{"price", 9}, {"qty", 100}}})}}}}));
    CHECK_FALSE(quote().healthy);
    now = 11;
    market->apply_price_book(book());
    CHECK_FALSE(quote().healthy);
}

TEST_CASE_FIXTURE(MarketFixture, "kafka offsets are deduplicated per topic partition") {
    CHECK(market->apply_price_book(book(), std::string("price-book"), 0, 4));
    CHECK(market->apply_volume(volume(1, 1000), std::string("trading-volume"), 0, 4));
    now = 1;
    CHECK_FALSE(market->apply_volume(volume(2, 2000), std::string("trading-volume"), 0, 4));
    CHECK(market->apply_volume(volume(2, 1100), std::string("trading-volume"), 0, 5));
    CHECK(earned() == Dec(10));
}

TEST_CASE_FIXTURE(MarketFixture, "forecast rate is bounded fresh and counts zero volume intervals") {
    market = fresh_adapter(3);
    market->apply_volume(volume(1, 1000));
    CHECK_FALSE(market->volume_rate("XYZ"));
    for (long long sequence = 2; sequence < 7; ++sequence) {
        now = static_cast<double>(sequence - 1);
        market->apply_volume(volume(sequence, 1000 + (sequence - 1) * 100));
    }
    REQUIRE(market->volume_rate("XYZ"));
    CHECK(*market->volume_rate("XYZ") == Dec(100));
    now = 6;
    market->apply_volume(volume(7, 1500));
    REQUIRE(market->volume_rate("XYZ"));
    CHECK(market->volume_rate("XYZ")->div(Decimal(1), 6) == Dec(200).div(Dec(3), 6));
    MESSAGE("rate=" << market->volume_rate("XYZ")->str());
    now = 20;
    CHECK_FALSE(market->volume_rate("XYZ"));
}

TEST_CASE_FIXTURE(MarketFixture, "new adapter after restart discards first cumulative snapshot") {
    market->apply_volume(volume(1, 1000));
    now = 1;
    market->apply_volume(volume(2, 1100));
    auto restarted = fresh_adapter();
    now = 2;
    restarted->apply_volume(volume(50, 99999));
    CHECK(earned() == Dec(10));
    now = 3;
    restarted->apply_volume(volume(51, 100099));
    CHECK(earned() == Dec(20));
}

TEST_CASE_FIXTURE(MarketFixture, "reconnect or restart cannot hide same session volume correction") {
    market->apply_volume(volume(1, 1000));
    now = 1;
    market->apply_volume(volume(2, 2000));
    CHECK(earned() == Dec(100));
    market->set_volume_connected(false);
    now = 2;
    market->set_volume_connected(true);
    auto restarted = fresh_adapter();
    restarted->apply_price_book(book());
    restarted->apply_volume(volume(1, 1900, {{"feed_id", "restarted-producer"}}));
    CHECK(engine->tick().empty());
    CHECK(reason().find("unknown tape correction") != std::string::npos);
    CHECK(restarted->market_status("XYZ").state == "UNKNOWN");
    CHECK((*engine->get_market_checkpoint("XYZ"))["counter_conflict"] == true);
}

TEST_CASE_FIXTURE(MarketFixture, "unknown or delayed flags do not earn credit") {
    CHECK(market->market_status("XYZ").state == "UNKNOWN");
    market->apply_volume(volume(1, 1000, {{"is_halted", nullptr}}));
    now = 1;
    market->apply_volume(volume(2, 2000, {{"is_halted", false}, {"is_delayed", nullptr}}));
    now = 2;
    market->apply_volume(volume(3, 3000, {{"is_delayed", true}}));
    CHECK(earned() == Dec(0));
    CHECK(market->market_status("XYZ").state == "UNKNOWN");
    now = 3;
    market->apply_volume(volume(4, 4000));
    CHECK(earned() == Dec(0));
    now = 4;
    market->apply_volume(volume(5, 4100));
    CHECK(earned() == Dec(10));
}

TEST_CASE_FIXTURE(MarketFixture, "halt requests cancel without releasing partial fill reservation") {
    market->apply_price_book(book());
    market->apply_volume(volume(1, 1000));
    now = 1;
    market->apply_volume(volume(2, 2000));
    json submit = engine->tick()[0];
    OrderReport r;
    r.status = "PARTIALLY_FILLED";
    r.cumulative_filled = 20;
    r.broker_order_id = "broker-1";
    engine->on_order_report(S(submit["client_order_id"]), r);
    now = 1.1;
    market->apply_volume(volume(3, nullptr, {{"is_halted", true}, {"healthy", false}}));
    market->apply_price_book(book());
    std::vector<json> actions = engine->tick();
    bool cancel = false;
    for (auto& a : actions) if (a["type"] == "cancel") cancel = true;
    CHECK(cancel);
    json p = engine->get_parent(parent);
    CHECK(Dec(p["reserved"]) == Dec(80));
    CHECK(Dec(p["filled"]) == Dec(20));
    CHECK(market->market_status("XYZ").state == "HALTED");
    r.cumulative_filled = 30;
    engine->on_order_report(S(submit["client_order_id"]), r);
    CHECK(Dec(engine->get_parent(parent)["reserved"]) == Dec(70));
    CHECK(earned() == Dec(100));
}

TEST_CASE_FIXTURE(MarketFixture, "reopening discards auction and waits for cooldown and fresh book") {
    market->apply_volume(volume(1, 1000));
    market->apply_price_book(book());
    now = 1;
    market->apply_volume(volume(2, nullptr, {{"is_halted", true}, {"healthy", false}}));
    json original_deadline = engine->get_parent(parent)["deadline"];
    now = 2;
    market->apply_volume(volume(3, 100000));
    CHECK(market->market_status("XYZ").state == "RESUMING");
    for (long long sequence = 4; sequence < 14; ++sequence) {
        now = static_cast<double>(sequence - 1);
        market->apply_price_book(book({{"kind", "PRICE_BOOK_HEARTBEAT"}}));
        market->apply_volume(volume(sequence, 100000 + sequence * 1000));
    }
    CHECK(market->market_status("XYZ").state == "RESUMING");
    CHECK(earned() == Dec(0));
    now = 13;
    market->apply_price_book(book({{"kind", "PRICE_BOOK"}}));
    market->apply_volume(volume(14, 120000));
    CHECK(market->market_status("XYZ").state == "TRADING");
    CHECK(earned() == Dec(0));
    now = 14;
    market->apply_volume(volume(15, 120100));
    CHECK(earned() == Dec(10));
    CHECK(engine->get_parent(parent)["deadline"] == original_deadline);
}

TEST_CASE_FIXTURE(MarketFixture, "stale unknown and unhealthy false flags cannot release halt") {
    market->apply_volume(volume(1, nullptr, {{"is_halted", true}, {"healthy", false}}));
    now = 1;
    market->apply_volume(volume(2, 1000, {{"is_halted", nullptr}}));
    CHECK(market->market_status("XYZ").state == "HALTED");
    now = 2;
    market->apply_volume(volume(3, nullptr, {{"healthy", false}}));
    CHECK(market->market_status("XYZ").state == "HALTED");
    now = 20;
    market->apply_volume(volume(4, 1000, {{"observed_at", iso(15)}}));
    CHECK(market->market_status("XYZ").state == "HALTED");
    now = 21;
    market->apply_volume(volume(5, 1000));
    market->apply_price_book(book());
    CHECK(market->market_status("XYZ").state == "RESUMING");
    now = 30;
    market->apply_volume(volume(6, 2000, {{"is_halted", nullptr}}));
    now = 31;
    market->apply_price_book(book());
    market->apply_volume(volume(7, 3000));
    CHECK(market->market_status("XYZ").state == "RESUMING");
    now = 32;
    market->apply_volume(volume(8, 4000));
    CHECK(market->market_status("XYZ").state == "TRADING");
    CHECK(earned() == Dec(0));
}

TEST_CASE_FIXTURE(MarketFixture, "halt latch survives adapter restart") {
    market->apply_volume(volume(1, nullptr, {{"is_halted", true}, {"healthy", false}}));
    auto restarted = fresh_adapter();
    CHECK(restarted->market_status("XYZ").state == "HALTED");
    now = 1;
    restarted->apply_volume(volume(2, 10000));
    CHECK(restarted->market_status("XYZ").state == "RESUMING");
    CHECK(earned() == Dec(0));
}

TEST_CASE_FIXTURE(MarketFixture, "new halt sequence latches even when observation timestamp is unchanged") {
    market->apply_volume(volume(1, 1000));
    market->apply_price_book(book());
    market->apply_volume(volume(2, nullptr, {{"is_halted", true}, {"healthy", false}}));
    CHECK(market->market_status("XYZ").state == "HALTED");
    CHECK_FALSE(quote().healthy);
}

TEST_CASE_FIXTURE(MarketFixture, "malformed trade correction holds even while halted") {
    market->apply_volume(volume(1, nullptr, {{"trades", json::array({{{"event_id", "trade-1"}, {"quantity", 100}, {"source_time", iso()}}})}}));
    now = 1;
    market->apply_volume(volume(2, nullptr, {{"is_halted", true}, {"healthy", false},
                                             {"trades", json::array({{{"event_id", "broken-correction"}, {"quantity", "NaN"}, {"correction_of", "trade-1"}}})}}));
    CHECK(reason().find("unknown tape correction") != std::string::npos);
    CHECK(earned() == Dec(10));
}
