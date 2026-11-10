// Smart buying (mode H) end to end through the runtime, engine and feedback correlator
// (tests/test_smart_buying.py).
#include "smart_fixtures.h"

#include <cmath>
#include <set>

#include "algo/smart_buying/runtime.h"
#include "model/market_data.h"

using namespace ngtest;
using ng::AccountDirectory;
using ng::AccountMeta;
using ng::AccountSnapshot;
using ng::BookLevel;
using ng::MarketDataStore;
using ng::OrderBookSnapshot;
using ng::smart::SmartBuyingEngine;
using ng::smart::SmartBuyingRuntime;
using ng::smart::ValueError;

namespace {

// Exposes the publisher handoff so a planned action can be dispatched after the test changed the
// account or the book, like the Python tests calling runtime._publish_action directly.
struct TestBuyingRuntime : SmartBuyingRuntime {
    using SmartBuyingRuntime::SmartBuyingRuntime;
    using SmartBuyingRuntime::publish_action;
};

struct BuyingFixture {
    TempDir dir{"smart-buying"};
    double clock = 1800000000.0;
    AccountDirectory metas{std::vector<AccountMeta>{meta("A", 1, "Tiger", "API")}};
    std::map<std::string, AccountSnapshot> snapshots;
    MarketDataStore md;
    std::vector<json> published;
    bool fast_owns = false;
    std::shared_ptr<SmartBuyingEngine> engine;
    std::unique_ptr<TestBuyingRuntime> runtime;
    json parent;

    BuyingFixture() {
        snapshots["A"] = account("A", 10000);
        OrderBookSnapshot book;
        book.symbol = "XYZ";
        book.asks.push_back(BookLevel{20.0, 10000.0, std::nullopt});
        book.asof_epoch = static_cast<long long>(clock);
        md.upsert_book(book);
        engine = std::make_shared<SmartBuyingEngine>(dir.file("buying.sqlite3"), [this] { return clock; }, [this] { return clock; });
        runtime = std::make_unique<TestBuyingRuntime>(
            ng::normalize_smart_buying_config(nullptr), md, [this]() -> const AccountDirectory& { return metas; },
            [this] { return snapshots; }, [this](const json& command, const std::string&) { published.push_back(command); },
            [this](const std::string&) { return fast_owns; }, engine, [this] { return clock; }, [this] { return clock; });
        runtime->set_feed_states_for_test({{"orders", "live"}, {"volume", "live"}});
    }
    ~BuyingFixture() { runtime->close(); }

    std::string iso() const { return ng::iso_utc_offset(clock); }
    AccountSnapshot account(const std::string& id, double cash) const {
        AccountSnapshot s;
        s.account_id = id;
        s.cash = cash;
        s.ts = iso();
        s.trading_enabled = true;
        return s;
    }

    json start(json settings = json::object()) {
        json merged = {{"account_ids", json::array({"A"})}, {"symbol", "XYZ"}, {"deadline", nullptr}};
        for (auto it = settings.begin(); it != settings.end(); ++it) merged[it.key()] = it.value();
        parent = runtime->start_parent(merged);
        quote();
        return parent;
    }

    void quote(const std::string& ask = "20", json size = 10000, std::optional<std::vector<json>> bid_prices = std::nullopt) {
        QuoteUpdate u;
        u.bid = (Decimal::parse(ask) - Decimal::parse(".01")).str();
        u.ask = ask;
        u.bid_size = 10000;
        u.ask_size = std::move(size);
        u.source_time = clock;
        u.bid_prices = std::move(bid_prices);
        engine->on_quote("XYZ", u);
    }

    void volume(json quantity, const std::string& event = "trade-1") {
        TradeUpdate t;
        t.quantity = std::move(quantity);
        t.event_id = event;
        t.source_time = clock;
        engine->on_trade("XYZ", t);
    }

    // The last published command, or null when nothing has been published yet.
    json tick() {
        runtime->tick();
        runtime->wait_idle();
        return published.empty() ? json(nullptr) : published.back();
    }

    void report(const json& command, const std::string& status = "FILLED", json filled = nullptr, long long sequence = 1,
                json extra = json::object()) {
        json payload = {{"schema_version", 1},
                        {"account_id", command["account_id"]},
                        {"order_id", command["command_id"]},
                        {"sequence", sequence},
                        {"created_at", command["ts"]},
                        {"updated_at", iso()},
                        {"symbol", "XYZ"},
                        {"side", "BUY"},
                        {"order_type", "LIMIT"},
                        {"status", status},
                        {"quantity", command["qty_shares"]},
                        {"filled_quantity", filled.is_null() ? command["qty_shares"] : filled},
                        {"average_fill_price", command["limit_price"]},
                        {"limit_price", command["limit_price"]},
                        {"command_id", command["command_id"]}};
        for (auto it = extra.begin(); it != extra.end(); ++it) payload[it.key()] = it.value();
        runtime->on_order_payload(payload.dump());
    }

    json status() {
        json p = engine->get_parent(S(parent["parent_id"]));
        CHECK(Dec(p["earned"]) == Dec(p["free_credit"]) + Dec(p["reserved"]) + Dec(p["filled"]) + Dec(p["discarded"]));
        return p;
    }
};

}  // namespace

TEST_CASE("fixed participation and minimum child") {
    struct Row { const char* mode; int volume; const char* expected; int quantity; };
    for (Row row : {Row{"patient", 2000, ".05", 100}, Row{"normal", 1250, ".08", 100}, Row{"urgent", 1000, ".15", 150}}) {
        CAPTURE(std::string(row.mode));
        BuyingFixture f;
        // Requests cannot override the fixed rates or lower the 100-share minimum.
        json p = f.start({{"mode", row.mode}, {"participation_percentage", 99}, {"min_quantity", 1}});
        CHECK(Dec(p["participation"]) == Dec(row.expected));
        CHECK(f.tick().is_null());  // Startup never invents tape credit.
        f.volume(row.volume);
        json command = f.tick();
        REQUIRE(command.is_object());
        CHECK(command["side"] == "BUY");
        CHECK(command["trading_mode"] == "H");
        CHECK(command["qty_shares"] == row.quantity);  // volume x fixed rate
        CHECK(command["limit_price"] == 20);
        CHECK(command["type"] == "LIMIT_ORDER");
    }
}

TEST_CASE_FIXTURE(BuyingFixture, "selling ultra mode is not available for buying") {
    CHECK(runtime->config().participation_percentages ==
          std::map<std::string, std::string>{{"patient", "5"}, {"normal", "8"}, {"urgent", "15"}});
    for (const char* mode : {"ultra", "4"}) {
        CAPTURE(std::string(mode));
        CHECK_THROWS_WITH_AS(start({{"mode", mode}}), "mode must be patient, normal, or urgent", ValueError);
        try {
            engine->start_parent(json{{"account_id", "A"}, {"symbol", "XYZ"}, {"quantity", "100"}, {"mode", mode}});
            FAIL("expected a ValueError");
        } catch (const ValueError& e) {
            CHECK(std::string(e.what()).find("mode must be") != std::string::npos);
        }
    }
}

TEST_CASE_FIXTURE(BuyingFixture, "subminimum credit accumulates without rounding up") {
    start();
    volume(1249);
    CHECK(tick().is_null());
    volume(1, "trade-2");
    CHECK(tick()["qty_shares"] == 100);
    volume(1250, "trade-2");  // Replay earns no more credit.
    CHECK(Dec(status()["earned"]) == Dec(100));
}

TEST_CASE_FIXTURE(BuyingFixture, "maximum price and actual ask liquidity") {
    start({{"maximum_price", "19.99"}});
    volume(10000);
    CHECK(tick().is_null());
    quote("19.99", 99);
    CHECK(tick().is_null());
    quote("19.99", 150);
    json command = tick();
    REQUIRE(command.is_object());
    CHECK(command["qty_shares"] == 150);
    CHECK(command["limit_price"].get<double>() <= 19.99);
}

TEST_CASE_FIXTURE(BuyingFixture, "cash resized at dispatch and limit never chases") {
    start({{"maximum_price", 20}});
    volume(10000);
    std::vector<json> actions = engine->tick();
    REQUIRE_FALSE(actions.empty());
    snapshots["A"].available_cash_by_currency = {{"USD", 2500}};
    quote("21", 10000, std::vector<json>{"20.99", "20.5", "20"});
    runtime->publish_action(actions[0]);
    REQUIRE_FALSE(published.empty());
    CHECK(published.back()["qty_shares"] == 125);
    CHECK(published.back()["limit_price"] == 20);
}

TEST_CASE_FIXTURE(BuyingFixture, "partial fills and cancel reserve cash until terminal") {
    snapshots["A"].cash = 5000;
    start();
    volume(10000);
    json command = tick();
    REQUIRE(command.is_object());
    CHECK(command["qty_shares"] == 250);
    report(command, "PARTIALLY_FILLED", 150);
    CHECK(status()["reserved"] == "100");
    CHECK(status()["accounts"]["A"]["spent"] == "3000");
    runtime->stop_parent(S(parent["parent_id"]));
    CHECK(runtime->owns_account("A"));
    report(command, "CANCELED", 175, 2);
    CHECK(status()["accounts"]["A"]["spent"] == "3500");
    CHECK_FALSE(runtime->owns_account("A"));
    CHECK(status()["reserved"] == "0");
}

TEST_CASE_FIXTURE(BuyingFixture, "one child at a time and subminimum tail stays unspent") {
    snapshots["A"].cash = 2500;
    start();
    volume(1250);
    json command = tick();
    REQUIRE(command.is_object());
    volume(10000, "trade-2");
    tick();
    CHECK(published.size() == 1);
    report(command);
    CHECK(status()["state"] == "COMPLETED");
    CHECK(status()["remaining"] == "25");
    CHECK(status()["accounts"]["A"]["spent"] == "2000");
}

TEST_CASE_FIXTURE(BuyingFixture, "each accounts cash caps its own rotating child") {
    snapshots["A"].cash = 2000;
    metas = AccountDirectory(std::vector<AccountMeta>{meta("A", 1, "Tiger", "API"), meta("B", 2, "Tiger", "API")});
    snapshots["B"] = account("B", 6000);
    start({{"account_ids", json::array({"A", "B"})}});
    volume(5000);
    json first = tick();
    REQUIRE(first.is_object());
    CHECK(first["account_id"] == "A");
    CHECK(first["qty_shares"] == 100);
    report(first);
    clock += 5;
    quote();
    json second = tick();
    CHECK(second["account_id"] == "B");
    CHECK(second["qty_shares"] == 300);
}

TEST_CASE_FIXTURE(BuyingFixture, "cash budget not reused when account snapshot lags") {
    start();
    quote("30");
    volume(10000);
    json command = tick();
    REQUIRE(command.is_object());
    CHECK(command["qty_shares"] == 333);
    report(command);
    clock += 5;
    quote("30");
    volume(10000, "trade-2");
    tick();
    CHECK(published.size() == 1);  // Only $10 remains, despite the old $10k snapshot.
}

TEST_CASE_FIXTURE(BuyingFixture, "stale and disabled accounts and owned accounts rejected") {
    snapshots["A"].trading_enabled = false;
    CHECK_THROWS_WITH_AS(start(), "Trading is disabled for this account", ValueError);
    snapshots["A"].trading_enabled = true;
    fast_owns = true;
    CHECK_THROWS_AS(start(), ValueError);
    fast_owns = false;
    snapshots["A"].ts = "2020-01-01T00:00:00Z";
    CHECK_THROWS_WITH_AS(start(), "Account cash snapshot is stale", ValueError);
}

TEST_CASE_FIXTURE(BuyingFixture, "scheduled start and deadline do not invent volume") {
    start({{"start_time", clock + 10}, {"deadline", clock + 20}});
    volume(10000);
    CHECK(tick().is_null());
    clock += 10;
    quote();
    CHECK(tick().is_null());
    volume(1250, "trade-2");
    json command = tick();
    REQUIRE(command.is_object());
    CHECK(command["qty_shares"] == 100);
    clock += 10;
    tick();
    CHECK(status()["state"] == "STOPPING");
    CHECK(status()["reserved"] == "100");
}

TEST_CASE_FIXTURE(BuyingFixture, "buy feedback rejects sell side and restart keeps reservation") {
    start();
    volume(1250);
    json command = tick();
    REQUIRE(command.is_object());
    CHECK_THROWS_AS(report(command, "FILLED", nullptr, 1, {{"side", "SELL"}}), ValueError);
    SmartBuyingEngine recovered(dir.file("buying.sqlite3"), [this] { return clock; }, [this] { return clock; });
    json restored = recovered.get_parent(S(parent["parent_id"]));
    CHECK(restored["reserved"] == "100");
    // Account snapshots always carry float cash (account_snapshots.py), so the persisted budget is
    // Decimal(str(10000.0)); the Python test passes an int and therefore sees "10000".
    CHECK(restored["accounts"]["A"]["cash_budget"] == "10000.0");
    for (const auto& action : recovered.tick()) CHECK(action.at("type") != "submit");
    recovered.close();
}

TEST_CASE_FIXTURE(BuyingFixture, "live volume adapter earns credit once and uses ask depth") {
    start();
    auto volume_payload = [this](long long sequence, long long total) {
        return json{{"schema_version", 1}, {"type", "TRADING_VOLUME"}, {"symbol", "XYZ"}, {"source", "TradeStation"},
                    {"feed_id", "live"}, {"sequence", sequence}, {"total_volume", total},
                    {"observed_at", ng::iso_utc_offset(clock)}, {"interval_start", ng::iso_utc_offset(clock - 1)},
                    {"session", "2027-01-15"}, {"healthy", true}, {"is_halted", false}, {"is_delayed", false}};
    };
    runtime->on_volume_payload(volume_payload(1, 10000));
    CHECK(Dec(status()["earned"]) == Dec(0));
    clock += 1;
    json update = volume_payload(2, 11250);
    runtime->on_volume_payload(update);
    runtime->on_volume_payload(update);
    runtime->on_price_book(json{{"symbol", "XYZ"},
                                {"update_time", static_cast<long long>(clock * 1e9)},
                                {"sequence_num", 1},
                                {"bid_side", {{"levels", json::array({{{"price", "19.99"}, {"quantity", "10"}}})}}},
                                {"offer_side", {{"levels", json::array({{{"price", "20"}, {"quantity", "150"}}})}}}});
    CHECK(Dec(status()["earned"]) == Dec(100));
    CHECK(tick()["qty_shares"] == 100);  // Uses ask liquidity, not 10% of the bid.
}

TEST_CASE_FIXTURE(BuyingFixture, "broker minimum above 100 is preserved") {
    runtime->mutable_config().instruments["XYZ"] = {{"min_quantity", "200"}, {"quantity_step", "100"}};
    start();
    volume(1250);
    CHECK(tick().is_null());
    volume(1250, "trade-2");
    CHECK(tick()["qty_shares"] == 200);
}
