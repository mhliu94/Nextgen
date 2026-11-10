#include "smart_fixtures.h"
#include "algo/smart_selling/feedback.h"

using namespace ngtest;
using ng::smart::SmartSellingFeedback;

namespace {

struct FeedbackFixture {
    TempDir dir{"smart-feedback"};
    double now = 0.0;
    double wall_start = 1800000000.0;
    std::string path;
    std::unique_ptr<SmartSellingEngine> engine;
    std::unique_ptr<SmartSellingFeedback> feedback;
    std::string parent;
    json child;

    FeedbackFixture() : path(dir.file("smart.sqlite3")) {
        engine = make_engine();
        feedback = std::make_unique<SmartSellingFeedback>(*engine);
        parent = engine->start_parent(json{{"account_id", "A"}, {"symbol", "XYZ"}, {"quantity", "1000"}, {"mode", "urgent"},
                                           {"horizon_seconds", 300}, {"tick_size", ".01"}, {"quantity_step", "1"},
                                           {"min_quantity", "1"}, {"min_notional", "0"}})["parent_id"].get<std::string>();
        quote();
        TradeUpdate t;
        t.quantity = "1000";
        t.event_id = "trade-1";
        t.source_time = wall_start;
        engine->on_trade("XYZ", t);
        for (auto& a : engine->tick()) if (a["type"] == "submit") child = a;
        REQUIRE(!child.is_null());
    }
    std::unique_ptr<SmartSellingEngine> make_engine() {
        return std::make_unique<SmartSellingEngine>(path, [this] { return now; }, [this] { return wall_start + now; });
    }
    void quote() {
        QuoteUpdate u;
        u.bid = "10";
        u.ask = "10.01";
        u.bid_size = "1000";
        u.source_time = wall_start + now;
        engine->on_quote("XYZ", u);
    }
    json status() { return engine->get_parent(parent); }
    json snapshot(json changes = json::object()) {
        std::string timestamp = ng::iso_utc(wall_start);
        json payload = {{"schema_version", 1}, {"account_id", "A"}, {"order_id", "broker-1"}, {"sequence", 1}, {"created_at", timestamp},
                        {"updated_at", timestamp}, {"symbol", "XYZ"}, {"side", "SELL"}, {"order_type", "LIMIT"}, {"status", "NEW"},
                        {"quantity", 100}, {"filled_quantity", 0}, {"average_fill_price", nullptr}, {"limit_price", 10},
                        {"command_id", child["client_order_id"]}};
        for (auto it = changes.begin(); it != changes.end(); ++it) payload[it.key()] = it.value();
        return payload;
    }
    bool apply(const json& payload) { return feedback->apply_snapshot(ng::smart::parse_smart_selling_snapshot(payload)); }
};

}  // namespace

TEST_CASE_FIXTURE(FeedbackFixture, "existing command id correlates without custom protocol") {
    CHECK(apply(snapshot()));
    CHECK(status()["active_order"]["broker_order_id"] == "broker-1");
    CHECK(Dec(status()["reserved"]) == Dec("100"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "existing client order id can correlate instead") {
    json payload = snapshot({{"client_order_id", child["client_order_id"]}});
    payload.erase("command_id");
    CHECK(feedback->apply_snapshot(payload.dump()));
}

TEST_CASE_FIXTURE(FeedbackFixture, "later broker id only snapshot uses persisted ownership") {
    apply(snapshot());
    json payload = snapshot({{"sequence", 2}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 40}, {"average_fill_price", 10}});
    payload.erase("command_id");
    CHECK(apply(payload));
    CHECK(Dec(status()["filled"]) == Dec("40"));
    CHECK(Dec(status()["reserved"]) == Dec("60"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "no guess from same symbol price quantity") {
    CHECK_FALSE(apply(snapshot({{"command_id", "unrelated-manual-order"}, {"order_id", "other-broker-order"}})));
    CHECK(status()["active_order"]["broker_order_id"].is_null());
    CHECK(Dec(status()["filled"]) == Dec("0"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "strict account symbol side type quantity and price matching") {
    for (json change : {json{{"account_id", "B"}}, json{{"symbol", "OTHER"}}, json{{"side", "BUY"}}, json{{"order_type", "MARKET"}},
                        json{{"quantity", 101}}, json{{"limit_price", 9.99}}}) {
        INFO(change.dump());
        CHECK_THROWS_AS(apply(snapshot(change)), std::invalid_argument);
        CHECK(Dec(status()["filled"]) == Dec("0"));
        CHECK(Dec(status()["reserved"]) == Dec("100"));
    }
}

TEST_CASE_FIXTURE(FeedbackFixture, "broker id cannot change after binding") {
    apply(snapshot());
    CHECK_THROWS_AS(apply(snapshot({{"sequence", 2}, {"order_id", "different-broker-id"}, {"status", "FILLED"}, {"filled_quantity", 100}})),
                    std::invalid_argument);
    CHECK(Dec(status()["reserved"]) == Dec("100"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "partial fill during cancel only final cumulative refunds leaves") {
    apply(snapshot({{"sequence", 1}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 40}}));
    apply(snapshot({{"sequence", 2}, {"status", "PENDING_CANCEL"}, {"filled_quantity", 60}}));
    CHECK(Dec(status()["filled"]) == Dec("60"));
    CHECK(Dec(status()["reserved"]) == Dec("40"));
    apply(snapshot({{"sequence", 3}, {"status", "CANCELED"}, {"filled_quantity", 60}}));
    CHECK(Dec(status()["filled"]) == Dec("60"));
    CHECK(Dec(status()["reserved"]) == Dec("0"));
    CHECK(Dec(status()["free_credit"]) == Dec(140));
}

TEST_CASE_FIXTURE(FeedbackFixture, "duplicate and reordered snapshots do not double count") {
    json partial = snapshot({{"sequence", 2}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 40}});
    apply(partial);
    apply(partial);
    apply(snapshot({{"sequence", 1}}));
    CHECK(Dec(status()["filled"]) == Dec("40"));
    json terminal = snapshot({{"sequence", 3}, {"status", "CANCELED"}, {"filled_quantity", 40}});
    apply(terminal);
    apply(terminal);
    CHECK(Dec(status()["free_credit"]) == Dec(160));
    CHECK(Dec(status()["reserved"]) == Dec("0"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "stale sequence with contradictory higher fill triggers core hold") {
    apply(snapshot({{"sequence", 2}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 40}}));
    apply(snapshot({{"sequence", 1}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 60}}));
    CHECK(status()["reason"].get<std::string>().find("conflicting reordered fill") != std::string::npos);
    CHECK(Dec(status()["filled"]) == Dec("40"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "newer lower fill triggers execution bust hold") {
    apply(snapshot({{"sequence", 1}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 40}}));
    apply(snapshot({{"sequence", 2}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 20}}));
    CHECK(status()["reason"].get<std::string>().find("execution bust") != std::string::npos);
    CHECK(Dec(status()["reserved"]) == Dec("60"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "unknown is not new fill authority or terminal evidence") {
    apply(snapshot({{"sequence", 2}, {"status", "UNKNOWN"}, {"filled_quantity", 40}}));
    CHECK(Dec(status()["filled"]) == Dec("0"));
    CHECK(Dec(status()["reserved"]) == Dec("100"));
    CHECK(status()["order_state"] == "RECONCILE");
    apply(snapshot({{"sequence", 1}, {"status", "CANCELED"}}));
    CHECK(Dec(status()["reserved"]) == Dec("100"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "reconciliation only replays known evidence") {
    CHECK_FALSE(feedback->reconcile(S(child["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec("100"));
    apply(snapshot());
    auto previous = feedback->latest_snapshot(S(child["client_order_id"]));
    now = 30;
    CHECK(feedback->reconcile(S(child["client_order_id"])));
    CHECK(previous == feedback->latest_snapshot(S(child["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec("100"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "restart replays terminal and reuses durable broker binding") {
    apply(snapshot({{"status", "PARTIALLY_FILLED"}, {"filled_quantity", 40}}));
    engine->close();
    feedback->close();
    engine = make_engine();
    feedback = std::make_unique<SmartSellingFeedback>(*engine);
    json terminal = snapshot({{"sequence", 2}, {"status", "CANCELED"}, {"filled_quantity", 60}});
    terminal.erase("command_id");
    CHECK(apply(terminal));
    CHECK(Dec(status()["filled"]) == Dec("60"));
    CHECK(Dec(status()["reserved"]) == Dec("0"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "archived duplicate cannot release newer child") {
    json terminal = snapshot({{"status", "CANCELED"}});
    apply(terminal);
    now = 5;
    quote();
    json second;
    for (auto& a : engine->tick()) if (a["type"] == "submit") second = a;
    REQUIRE(!second.is_null());
    CHECK(second["client_order_id"] != child["client_order_id"]);
    apply(terminal);
    CHECK(Dec(status()["reserved"]) == Dec("100"));
    CHECK(status()["active_order"]["client_order_id"] == second["client_order_id"]);
    apply(snapshot({{"sequence", 2}, {"status", "CANCELED"}, {"filled_quantity", 10}}));
    CHECK(status()["reason"].get<std::string>().find("conflicting archived fill") != std::string::npos);
    CHECK(Dec(status()["reserved"]) == Dec("100"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "snapshot schema rejects inconsistent terminal and unknown fields") {
    CHECK_THROWS_AS(ng::smart::parse_smart_selling_snapshot(snapshot({{"status", "FILLED"}, {"filled_quantity", 99}})), std::invalid_argument);
    CHECK_THROWS_AS(ng::smart::parse_smart_selling_snapshot(snapshot({{"authoritative", true}})), std::invalid_argument);
    CHECK_THROWS_AS(ng::smart::parse_smart_selling_snapshot(std::string("{\"filled_quantity\": NaN}")), std::invalid_argument);
}

TEST_CASE_FIXTURE(FeedbackFixture, "source time and original creation time remain consistent") {
    apply(snapshot({{"updated_at", "2027-01-15T08:00:01+00:00"}}));
    CHECK_THROWS_AS(apply(snapshot({{"sequence", 2}})), std::invalid_argument);
    CHECK_THROWS_AS(apply(snapshot({{"sequence", 2}, {"created_at", "2027-01-15T08:00:01+00:00"}, {"updated_at", "2027-01-15T08:00:01+00:00"}})),
                    std::invalid_argument);
}

TEST_CASE_FIXTURE(FeedbackFixture, "latest snapshot returns a copy") {
    apply(snapshot());
    auto saved = feedback->latest_snapshot(S(child["client_order_id"]));
    (*saved)["filled_quantity"] = 100;
    CHECK((*feedback->latest_snapshot(S(child["client_order_id"])))["filled_quantity"] == 0);
}

TEST_CASE_FIXTURE(FeedbackFixture, "manual open orders are tracked without owning the child") {
    json manual = snapshot({{"command_id", "manual-1"}, {"order_id", "manual-broker-1"}});
    CHECK_FALSE(apply(manual));
    CHECK(feedback->has_open_orders("A", "XYZ"));
    CHECK_FALSE(feedback->has_open_orders("B", "XYZ"));
    CHECK_FALSE(feedback->has_open_orders("A", "OTHER"));
    json closed = manual;
    closed["sequence"] = 2;
    closed["status"] = "CANCELED";
    apply(closed);
    CHECK_FALSE(feedback->has_open_orders("A", "XYZ"));
    apply(manual);
    CHECK_FALSE(feedback->has_open_orders("A", "XYZ"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "unknown manual order is an unresolved commitment") {
    apply(snapshot({{"command_id", "manual"}, {"status", "UNKNOWN"}}));
    CHECK(feedback->has_open_orders("A", "XYZ"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "terminal sequence watermark survives feedback restart") {
    feedback->close();
    std::string feedback_path = dir.file("feedback.sqlite3");
    feedback = std::make_unique<SmartSellingFeedback>(*engine, 10000, feedback_path);
    json manual = snapshot({{"command_id", "manual-1"}, {"order_id", "manual-broker-1"}});
    json closed = manual;
    closed["sequence"] = 2;
    closed["status"] = "CANCELED";
    apply(closed);
    feedback->close();
    feedback = std::make_unique<SmartSellingFeedback>(*engine, 10000, feedback_path);
    apply(manual);
    CHECK_FALSE(feedback->has_open_orders("A", "XYZ"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "pending manual handoff requires exact terminal correlation") {
    json command = {{"command_id", "manual-pending"}, {"account_id", "A"}, {"symbol", "US.XYZ"}};
    feedback->track_external_order(command);
    CHECK(feedback->has_open_orders("A", "XYZ"));
    apply(snapshot({{"command_id", "someone-else"}, {"order_id", "unrelated"}, {"status", "CANCELED"}}));
    CHECK(feedback->has_open_orders("A", "XYZ"));
    apply(snapshot({{"command_id", "manual-pending"}, {"account_id", "B"}, {"order_id", "wrong"}, {"status", "CANCELED"}}));
    CHECK(feedback->has_open_orders("A", "XYZ"));
    apply(snapshot({{"command_id", "manual-pending"}, {"order_id", "manual-broker"}, {"status", "CANCELED"}}));
    CHECK_FALSE(feedback->has_open_orders("A", "XYZ"));
    feedback->track_external_order(command);
    CHECK_FALSE(feedback->has_open_orders("A", "XYZ"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "pending manual handoff and terminal watermark are durable") {
    feedback->close();
    std::string p = dir.file("feedback.sqlite3");
    feedback = std::make_unique<SmartSellingFeedback>(*engine, 10000, p);
    feedback->track_external_order(json{{"command_id", "manual"}, {"account_id", "A"}, {"symbol", "XYZ"}});
    feedback->close();
    feedback = std::make_unique<SmartSellingFeedback>(*engine, 10000, p);
    CHECK(feedback->has_open_orders("A", "XYZ"));
    apply(snapshot({{"command_id", "manual"}, {"status", "NEW"}, {"sequence", 2}}));
    apply(snapshot({{"command_id", "manual"}, {"status", "CANCELED"}, {"sequence", 1}}));
    CHECK(feedback->has_open_orders("A", "XYZ"));
    apply(snapshot({{"command_id", "manual"}, {"status", "CANCELED"}, {"sequence", 3}}));
    CHECK_FALSE(feedback->has_open_orders("A", "XYZ"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "prefixed manual symbols block same canonical instrument") {
    apply(snapshot({{"command_id", "manual"}, {"symbol", "US.XYZ"}}));
    CHECK(feedback->has_open_orders("A", "XYZ"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "one terminal snapshot cannot clear two external commands") {
    for (const char* identity : {"first-manual", "second-manual"}) {
        feedback->track_external_order(json{{"command_id", identity}, {"account_id", "A"}, {"symbol", "XYZ"}});
    }
    CHECK_THROWS_AS(apply(snapshot({{"command_id", "first-manual"}, {"client_order_id", "second-manual"}, {"status", "CANCELED"}})),
                    std::invalid_argument);
    CHECK(feedback->has_open_orders("A", "XYZ"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "archived metadata conflict holds newer parent without refunding") {
    apply(snapshot({{"status", "CANCELED"}}));
    engine->request_stop(parent);
    std::string next_parent = engine->start_parent(json{{"account_id", "A"}, {"symbol", "XYZ"}, {"quantity", "1000"}, {"mode", "urgent"},
                                                        {"horizon_seconds", 300}, {"tick_size", ".01"}, {"quantity_step", "1"},
                                                        {"min_quantity", "1"}})["parent_id"].get<std::string>();
    quote();
    TradeUpdate t;
    t.quantity = "1000";
    t.event_id = "next-parent-print";
    t.source_time = wall_start;
    engine->on_trade("XYZ", t);
    engine->tick();
    CHECK_THROWS_AS(apply(snapshot({{"sequence", 2}, {"status", "CANCELED"}, {"filled_quantity", 10}, {"limit_price", 9.99}})),
                    std::invalid_argument);
    json current = engine->get_parent(next_parent);
    CHECK(current["reason"].get<std::string>().find("conflicting order metadata") != std::string::npos);
    CHECK(Dec(current["reserved"]) == Dec("100"));
    CHECK(Dec(current["filled"]) == Dec("0"));
}

TEST_CASE_FIXTURE(FeedbackFixture, "wrong account cannot quarantine a different account's child") {
    CHECK_THROWS_AS(apply(snapshot({{"account_id", "B"}})), std::invalid_argument);
    CHECK(status()["reason"].get<std::string>().find("conflicting order") == std::string::npos);
}
