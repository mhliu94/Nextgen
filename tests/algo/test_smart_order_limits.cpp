// G/H order guards exercise real reservations, depth snapshots and dispatch
// (tests/test_smart_order_limits.py). Every side-neutral case runs for both engines.
#include "smart_fixtures.h"

#include "algo/smart_buying/engine.h"
#include "algo/smart_selling/market_adapter.h"

using namespace ngtest;
using ng::smart::SmartBuyingEngine;
using ng::smart::SmartSellingMarketAdapter;

namespace {

constexpr double kWall = 1800000000.0;

struct LimitsHarness {
    TempDir dir{"smart-limits"};
    bool buy;
    double now = 0.0;
    std::unique_ptr<SmartSellingEngine> engine;

    explicit LimitsHarness(bool buying) : buy(buying) { reopen(); }
    void reopen() {
        std::string path = dir.file("orders.sqlite3");
        auto clock = [this] { return now; };
        auto wall = [] { return kWall; };
        if (buy) engine = std::make_unique<SmartBuyingEngine>(path, clock, wall);
        else engine = std::make_unique<SmartSellingEngine>(path, clock, wall);
    }

    // The user's price bound on this side: a minimum when selling, a maximum when buying.
    json bound(const char* sell_value, const char* buy_value) const {
        return buy ? json{{"maximum_price", buy_value}} : json{{"minimum_price", sell_value}};
    }

    std::string start(json overrides = json::object()) {
        json settings = {{"account_id", "A"},  {"symbol", "XYZ"},     {"quantity", "1000"},     {"mode", "urgent"},
                         {"deadline", nullptr}, {"tick_size", ".01"}, {"quantity_step", "1"},   {"min_quantity", "100"},
                         {"min_notional", "0"}, {"account_budgets", {{"A", "100000"}}}};
        for (auto it = overrides.begin(); it != overrides.end(); ++it) settings[it.key()] = it.value();
        json parent = engine->start_parent(settings);
        if (buy) static_cast<SmartBuyingEngine&>(*engine).set_cash_limit("A", Dec(100000));
        quote();
        return S(parent["parent_id"]);
    }

    void quote(json overrides = json::object()) {
        json values = {{"bid", "10"},
                       {"ask", "10.1"},
                       {"bid_size", 10000},
                       {"ask_size", 10000},
                       {"bid_prices", json::array({"10", "9.9", "9.7", "9.3", "8.5", "6.9"})},
                       {"ask_prices", json::array({"10.1", "10.2", "10.4", "10.8", "11.6", "13.2"})}};
        for (auto it = overrides.begin(); it != overrides.end(); ++it) values[it.key()] = it.value();
        QuoteUpdate u;
        u.bid = values["bid"];
        u.ask = values["ask"];
        u.bid_size = values["bid_size"];
        u.ask_size = values["ask_size"];
        u.bid_prices = values["bid_prices"].get<std::vector<json>>();
        u.ask_prices = values["ask_prices"].get<std::vector<json>>();
        u.source_time = kWall;
        engine->on_quote("XYZ", u);
    }

    void fund(const std::string& amount = "10000", const std::string& event = "tape") {
        TradeUpdate t;
        t.quantity = amount;
        t.event_id = event;
        t.source_time = kWall;
        engine->on_trade("XYZ", t);
    }

    std::vector<json> submits() {
        std::vector<json> out;
        for (auto& action : engine->tick()) {
            if (action.at("type") == "submit") out.push_back(action);
        }
        return out;
    }
};

template <typename Body>
void for_each_side(Body body) {
    for (bool buy : {false, true}) {
        CAPTURE(buy);
        LimitsHarness harness(buy);
        body(harness);
    }
}

bool contains(const json& text, const char* needle) { return text.get<std::string>().find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("five displayed levels block without reserving or spending credit") {
    for_each_side([](LimitsHarness& h) {
        std::string parent = h.start(h.bound("11.6", "8.5"));
        h.fund();
        json before = h.engine->get_parent(parent);
        for (int i = 0; i < 3; ++i) CHECK(h.engine->tick().empty());
        json after = h.engine->get_parent(parent);
        CHECK(contains(after["reason"], "five displayed levels"));
        CHECK(after["reserved"] == "0");
        CHECK(after["free_credit"] == before["free_credit"]);
        for (const auto& event : h.engine->events(parent)) CHECK(event.at("kind") != "submit_intent");
        // The bound becomes marketable: accumulated credit can now fund an order.
        if (h.buy) h.quote({{"bid", "8.4"}, {"ask", "8.5"}, {"bid_prices", json::array({"8.4"})}});
        else h.quote({{"bid", "11.6"}, {"ask", "11.7"}, {"ask_prices", json::array({"11.7"})}});
        std::vector<json> actions = h.engine->tick();
        REQUIRE_FALSE(actions.empty());
        CHECK(actions[0]["type"] == "submit");
    });
}

TEST_CASE("dispatch rechecks real levels and refunds blocked intents") {
    for (int distance : {4, 5, 6}) {
        for_each_side([distance](LimitsHarness& h) {
            CAPTURE(distance);
            std::string parent = h.start(h.bound("11.6", "8.5"));
            // Plan while the user bound is at the market.
            if (h.buy) h.quote({{"bid", "8.4"}, {"ask", "8.5"}});
            else h.quote({{"bid", "11.6"}, {"ask", "11.7"}});
            h.fund();
            std::vector<json> actions = h.engine->tick();
            REQUIRE_FALSE(actions.empty());
            json action = actions[0];
            json reserved = h.engine->get_parent(parent);
            // Gaps are deliberately far larger than a tick; repeated prices count once.
            std::vector<std::string> near = h.buy ? std::vector<std::string>{"10", "9.9", "9.7", "9.3", "9.0"}
                                                  : std::vector<std::string>{"10.1", "10.2", "10.4", "10.8", "11.0"};
            std::vector<std::string> levels(near.begin(), near.begin() + (distance - 1));
            for (const char* far : h.buy ? std::initializer_list<const char*>{"8.5", "6.9"}
                                         : std::initializer_list<const char*>{"11.6", "13.2"}) {
                levels.push_back(far);
            }
            json displayed = json::array();
            for (auto it = levels.rbegin(); it != levels.rend(); ++it) displayed.push_back(*it);
            for (int i = 0; i < 5; ++i) displayed.push_back(levels[0]);
            h.quote({{h.buy ? "bid_prices" : "ask_prices", displayed}});
            auto prepared = h.engine->prepare_dispatch(S(action["client_order_id"]));
            json after = h.engine->get_parent(parent);
            if (distance < 5) {
                REQUIRE(prepared);
                CHECK((*prepared)["limit_price"] == action["limit_price"]);
            } else {
                CHECK_FALSE(prepared);
                CHECK(after["reserved"] == "0");
                CHECK(Dec(after["free_credit"]) == Dec(reserved["free_credit"]) + Dec(reserved["reserved"]));
                CHECK(contains(after["reason"], "five displayed levels"));
                CHECK(h.engine->tick().empty());
            }
        });
    }
}

TEST_CASE("missing deep levels wait instead of guessing ticks") {
    for_each_side([](LimitsHarness& h) {
        std::string parent = h.start(h.bound("11.6", "8.5"));
        h.quote({{"bid_prices", json::array({"10", "9.9"})}, {"ask_prices", json::array({"10.1", "10.2"})}});
        h.fund();
        CHECK(h.engine->tick().empty());
        CHECK(contains(h.engine->get_parent(parent)["reason"], "waiting for displayed depth"));
    });
}

TEST_CASE("no bound or a marketable bound does not require a deep book") {
    for (bool with_bound : {false, true}) {
        for_each_side([with_bound](LimitsHarness& h) {
            CAPTURE(with_bound);
            h.start(with_bound ? h.bound("1", "100") : json::object());
            h.quote({{"bid_prices", json::array()}, {"ask_prices", json::array()}});
            h.fund();
            std::vector<json> actions = h.engine->tick();
            REQUIRE_FALSE(actions.empty());
            CHECK(actions[0]["type"] == "submit");
        });
    }
}

TEST_CASE("final dispatch respects side specific depth limits") {
    for_each_side([](LimitsHarness& h) {
        std::string parent = h.start();
        h.fund();
        std::vector<json> actions = h.engine->tick();
        REQUIRE_FALSE(actions.empty());
        json action = actions[0];
        json before = h.engine->get_parent(parent);
        h.quote({{"bid_size", 990}, {"ask_size", 99}});
        auto prepared = h.engine->prepare_dispatch(S(action["client_order_id"]));
        json after = h.engine->get_parent(parent);
        if (!h.buy) {
            // Selling has no displayed-depth cap: the funded child is sent unchanged.
            REQUIRE(prepared);
            CHECK(Dec((*prepared)["quantity"]) == Dec(100));
            CHECK(after["reserved"] == before["reserved"]);
            CHECK(after["free_credit"] == before["free_credit"]);
        } else {
            // Buying never bids for more than the displayed ask; 99 shares is below the minimum.
            CHECK_FALSE(prepared);
            CHECK(after["reserved"] == "0");
            CHECK(Dec(after["free_credit"]) == Dec(before["free_credit"]) + Dec(before["reserved"]));
        }
    });
}

TEST_CASE("selling accumulates 100 credit even after the partial flush timer") {
    // Upstream wrote this case for a 15% urgent rate (666 shares -> 99.9 credit); at the current 20%
    // rate the same boundary is 499 shares -> 99.8 credit.
    LimitsHarness h(false);
    std::string parent = h.start({{"min_quantity", "1"}});
    h.fund("499");
    h.now = 30;
    h.quote();
    CHECK(h.engine->tick().empty());
    CHECK(Dec(h.engine->get_parent(parent)["free_credit"]) == Dec("99.8"));
    h.fund("1", "extra");
    std::vector<json> actions = h.engine->tick();
    REQUIRE_FALSE(actions.empty());
    CHECK(Dec(actions[0]["quantity"]) == Dec(100));
}

TEST_CASE("existing small slice is upgraded without releasing the working reservation") {
    LimitsHarness h(false);
    std::string parent = h.start();
    h.fund();
    std::vector<json> actions = h.engine->tick();
    REQUIRE_FALSE(actions.empty());
    json child = actions[0];
    h.engine->close();
    {
        ng::Sqlite db(h.dir.file("orders.sqlite3"));
        ng::Statement select(db, "SELECT data FROM smart_parents WHERE id=?");
        select.bind(1, parent);
        REQUIRE(select.step());
        json stored = json::parse(select.column_text(0));
        stored["min_quantity"] = "1";
        stored["child_cap"] = "10";
        stored["base_child_cap"] = "10";
        ng::Statement update(db, "UPDATE smart_parents SET data=? WHERE id=?");
        update.bind(1, stored.dump());
        update.bind(2, parent);
        update.exec();
    }
    h.reopen();
    json recovered = h.engine->get_parent(parent);
    CHECK(Dec(recovered["child_cap"]) == Dec(100));
    CHECK(recovered["reserved"] == child["quantity"]);
    CHECK(recovered["active_order"]["client_order_id"] == child["client_order_id"]);
    h.quote();
    for (const auto& action : h.engine->tick()) CHECK(action.at("type") != "submit");
}

TEST_CASE("adapter counts distinct positive displayed levels") {
    LimitsHarness h(false);
    std::string parent = h.start({{"minimum_price", "11.6"}});
    SmartSellingMarketAdapter adapter(*h.engine, [] { return 0.0; }, [] { return kWall; });
    json offers = json::array();
    for (const char* price : {"11.6", "10.1", "10.4", "10.8", "10.1"}) offers.push_back({{"prc", price}, {"qty", 10000}});
    offers.push_back({{"price", "10.2"}, {"quantity", 0}});
    offers.push_back({{"price", "10.3"}, {"quantity", "bad"}});
    adapter.apply_price_book(json{{"symbol", "XYZ"},
                                  {"book_observed_at", kWall},
                                  {"bid_side", {{"levels", json::array({{{"price", "10"}, {"quantity", 10000}}})}}},
                                  {"offer_side", {{"levels", offers}}}});
    // Enable the independent tape feed after applying this depth snapshot.
    h.engine->set_feed_health("XYZ", true);
    h.fund();
    std::vector<json> actions = h.engine->tick();
    REQUIRE_FALSE(actions.empty());
    CHECK(actions[0]["type"] == "submit");  // Four real prices, despite gaps/duplicates.
    h.engine->abort_dispatch(S(h.engine->get_parent(parent)["active_order"]["client_order_id"]), "test cleanup");
}
