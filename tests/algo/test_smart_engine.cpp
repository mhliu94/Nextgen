#include "smart_fixtures.h"

#include <algorithm>

using namespace ngtest;

TEST_CASE_FIXTURE(EngineFixture, "three fixed modes and one tick midpoint") {
    struct Row { const char* mode; const char* rate; const char* price; };
    for (Row row : {Row{"patient", ".03", "10.01"}, Row{"normal", ".06", "10.01"}, Row{"urgent", ".10", "10"}}) {
        INFO(row.mode);
        start({{"mode", row.mode}, {"account_id", row.mode}});
        tape("10000", row.mode);
        json c = child();
        CHECK(Dec(c["limit_price"]) == Dec(row.price));
        CHECK(Dec(status()["participation"]) == Dec(row.rate));
        report(c, "FILLED", c["quantity"].get<std::string>(), {{"final", true}});
        engine->request_stop(parent);
    }
}

TEST_CASE_FIXTURE(EngineFixture, "configured participation is exact and survives restart and correction") {
    start({{"mode", "patient"}, {"participation_percentage", "0.000000000000000000000001"}});
    tape("1e24", "original");
    CHECK(Dec(status()["participation"]) == Dec("1e-26"));
    CHECK(Dec(status()["earned"]) == Dec(".01"));
    engine->close();
    reopen();
    CHECK(Dec(status()["participation"]) == Dec("1e-26"));
    correction("-1e23", "correction", "original");
    CHECK(Dec(status()["earned"]) == Dec(".009"));
    tape("1e23", "next");
    CHECK(Dec(status()["earned"]) == Dec(".01"));
}

TEST_CASE_FIXTURE(EngineFixture, "participation accepts full volume and rejects invalid values") {
    for (json percentage : {json(true), json(false), json(nullptr), json("NaN"), json("Infinity"), json(0), json(-1), json("100.01"), json("1e-25")}) {
        INFO(percentage.dump());
        CHECK_THROWS_AS(start({{"participation_percentage", percentage}}), ng::smart::ValueError);
    }
    start({{"participation_percentage", "100"}});
    tape("100");
    CHECK(Dec(status()["participation"]) == Dec(1));
    CHECK(Dec(status()["earned"]) == Dec(100));
    CHECK(Dec(child()["quantity"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "one parent per account and symbol") {
    start();
    CHECK_THROWS_WITH(start(), "an active Smart selling parent already owns this account/instrument");
}

TEST_CASE_FIXTURE(EngineFixture, "no invented initial credit") {
    start();
    CHECK(engine->tick().empty());
    now = 30;
    quote();
    CHECK(engine->tick().empty());
    CHECK(Dec(status()["earned"]) == Dec("0"));
}

TEST_CASE_FIXTURE(EngineFixture, "child and credit cap limit large prints") {
    start();
    tape("10000");
    json c = child();
    CHECK(Dec(c["quantity"]) == Dec(100));
    json s = status();
    CHECK(Dec(s["free_credit"]) == Dec(100));
    CHECK(Dec(s["discarded"]) == Dec(800));
    CHECK(engine->tick().empty());
}

TEST_CASE_FIXTURE(EngineFixture, "depth cap and partial budget flush") {
    start();
    quote({{"bid_size", "800"}});
    tape("600");
    CHECK(engine->tick().empty());
    now = 29;
    quote({{"bid_size", "800"}});
    CHECK(engine->tick().empty());
    now = 30;
    quote({{"bid_size", "800"}});
    CHECK(Dec(child()["quantity"]) == Dec(60));
}

TEST_CASE_FIXTURE(EngineFixture, "fractional credit and quantity step never round up") {
    start({{"quantity", "100"}, {"quantity_step", ".1"}, {"min_quantity", ".1"}, {"horizon_seconds", 300}});
    quote({{"bid_size", "100"}});
    tape("12.345");
    now = 30;
    quote({{"bid_size", "100"}});
    json c = child();
    CHECK(Dec(c["quantity"]) == Dec("1.2"));
    CHECK(Dec(status()["free_credit"]) == Dec(".0345"));
}

TEST_CASE_FIXTURE(EngineFixture, "partial fill during cancel refunds final leaves once") {
    start();
    tape();
    json c = child();
    now = 0.6;
    report(c, "PARTIALLY_FILLED", "40", {{"sequence", 1}});
    now = 2;
    CHECK(action_types(engine->tick()) == std::vector<std::string>{"cancel"});
    CHECK(Dec(status()["reserved"]) == Dec(60));
    now = 2.4;
    report(c, "PARTIALLY_FILLED", "60", {{"sequence", 2}});
    now = 2.6;
    report(c, "CANCELED", "60", {{"sequence", 3}, {"final", true}});
    report(c, "CANCELED", "60", {{"sequence", 3}, {"final", true}});
    report(c, "PARTIALLY_FILLED", "40", {{"sequence", 1}});
    json result = status();
    CHECK(Dec(result["filled"]) == Dec(60));
    CHECK(Dec(result["free_credit"]) == Dec(40));
    CHECK(Dec(result["reserved"]) == Dec(0));
    CHECK(Dec(result["fills_after_cancel"]) == Dec(20));
}

TEST_CASE_FIXTURE(EngineFixture, "delayed ack queues cancel and does not reset age") {
    start();
    tape();
    json c = child();
    now = 2;
    CHECK(engine->tick().empty());
    now = 3;
    CHECK(action_types(engine->tick()) == std::vector<std::string>{"query"});
    now = 4;
    report(c, "NEW", "0", {{"sequence", 1}});
    CHECK(action_types(engine->drain_actions()) == std::vector<std::string>{"cancel"});
    CHECK(Dec(status()["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "unknown submit and not found never refund") {
    start();
    tape();
    json c = child();
    engine->on_action_error(S(c["client_order_id"]), "submit", "lost connection");
    CHECK(action_types(engine->drain_actions()) == std::vector<std::string>{"query"});
    OrderReport r;
    r.status = "NOT_FOUND";
    r.cumulative_filled = "0";
    r.sequence = 1;
    engine->on_order_report(S(c["client_order_id"]), r);
    now = 4;
    CHECK(engine->tick().empty());
    now = 5;
    CHECK(action_types(engine->tick()) == std::vector<std::string>{"query"});
    CHECK(Dec(status()["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "missing final cumulative proof preserves reservation") {
    start();
    tape();
    json c = child();
    report(c, "CANCELED", "10", {{"final", false}});
    json result = status();
    CHECK(Dec(result["filled"]) == Dec(10));
    CHECK(Dec(result["reserved"]) == Dec(90));
    CHECK(Dec(result["free_credit"]) == Dec(0));
}

TEST_CASE_FIXTURE(EngineFixture, "inconsistent filled status does not refund missing quantity") {
    start();
    tape();
    json c = child();
    report(c, "FILLED", "40", {{"final", true}});
    json s = status();
    CHECK(Dec(s["filled"]) == Dec(40));
    CHECK(Dec(s["reserved"]) == Dec(60));
    CHECK(s["order_state"] == "RECONCILE");
}

TEST_CASE_FIXTURE(EngineFixture, "non authoritative report does not book fill") {
    start();
    tape();
    json c = child();
    report(c, "FILLED", "100", {{"authoritative", false}, {"final", true}});
    json result = status();
    CHECK(Dec(result["filled"]) == Dec(0));
    CHECK(Dec(result["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "stale and reordered reports do not double fill") {
    start();
    tape();
    json c = child();
    report(c, "PARTIALLY_FILLED", "40", {{"sequence", 3}});
    report(c, "PARTIALLY_FILLED", "20", {{"sequence", 2}});
    report(c, "PARTIALLY_FILLED", "40", {{"sequence", 3}});
    CHECK(Dec(status()["filled"]) == Dec(40));
}

TEST_CASE_FIXTURE(EngineFixture, "newer cumulative regression holds for execution bust") {
    start();
    tape();
    json c = child();
    report(c, "PARTIALLY_FILLED", "40", {{"sequence", 3}});
    report(c, "PARTIALLY_FILLED", "20", {{"sequence", 4}});
    CHECK(status()["reason"].get<std::string>().find("execution bust") != std::string::npos);
    CHECK(Dec(status()["filled"]) == Dec(40));
}

TEST_CASE_FIXTURE(EngineFixture, "archived conflict does not release new slot") {
    start();
    tape("2000");
    json first = child();
    report(first, "CANCELED", "40", {{"sequence", 1}, {"final", true}});
    now = 5;
    quote();
    json second = child();
    report(first, "CANCELED", "50", {{"sequence", 2}, {"final", true}});
    json result = status();
    CHECK(result["active_order"]["client_order_id"] == second["client_order_id"]);
    CHECK(Dec(result["reserved"]) == Dec(100));
    CHECK(result["reason"].get<std::string>().find("archived") != std::string::npos);
}

TEST_CASE_FIXTURE(EngineFixture, "tape dedup, corrections keep the original rate and fills earn no credit") {
    start({{"mode", "normal"}});
    tape("1000");
    tape("1000");
    correction("-100", "correction-1", "trade-1");
    correction("-100", "correction-1", "trade-1");
    CHECK(Dec(status()["earned"]) == Dec(54));
    now = 30;
    quote();
    json c = child();
    report(c, "FILLED", c["quantity"].get<std::string>(), {{"final", true}});
    CHECK(Dec(status()["earned"]) == Dec(54));
}

TEST_CASE_FIXTURE(EngineFixture, "negative correction keeps live reservation and signed credit") {
    start();
    tape();
    json c = child();
    correction("-900", "correction-1", "trade-1");
    json result = status();
    CHECK(Dec(result["free_credit"]) == Dec(-90));
    CHECK(Dec(result["reserved"]) == Dec(100));
    report(c, "FILLED", "100", {{"final", true}});
    now = 5;
    quote();
    CHECK(engine->tick().empty());
    CHECK(status()["reason"].get<std::string>().find("negative credit") != std::string::npos);
}

TEST_CASE_FIXTURE(EngineFixture, "unknown correction holds without guessing") {
    start();
    correction("-100", "correction", "missing");
    tape();
    CHECK(engine->tick().empty());
    CHECK(status()["reason"].get<std::string>().find("unknown tape correction") != std::string::npos);
}

TEST_CASE_FIXTURE(EngineFixture, "send spacing and one shot passive fallback") {
    start({{"mode", "patient"}});
    tape("10000");
    json first = child();
    report(first, "CANCELED", "20", {{"final", true}});
    CHECK(status()["fallback"] == true);
    now = 4;
    quote();
    CHECK(engine->tick().empty());
    now = 5;
    json second = child();
    CHECK(Dec(second["limit_price"]) == Dec(10));
    report(second, "CANCELED", "0", {{"final", true}});
    CHECK(status()["fallback"] == false);
}

TEST_CASE_FIXTURE(EngineFixture, "behind progress changes price not participation") {
    start({{"mode", "normal"}});
    tape("2000");
    now = 31;
    quote();
    json c = child();
    CHECK(Dec(c["limit_price"]) == Dec(10));
    CHECK(Dec(status()["participation"]) == Dec(".06"));
    CHECK(Dec(status()["earned"]) == Dec(120));
}

TEST_CASE_FIXTURE(EngineFixture, "price floor upward tick and tiny depth") {
    start({{"minimum_price", "10.025"}});
    tape();
    quote({{"bid_size", "9"}});
    CHECK(engine->tick().empty());
    quote({{"bid_size", "1000"}});
    json c = child();
    CHECK(Dec(c["limit_price"]) == Dec("10.03"));
    CHECK(status()["reason"].get<std::string>().find("minimum price") != std::string::npos);
}

TEST_CASE_FIXTURE(EngineFixture, "stale sources and sequence gap block until snapshot") {
    start();
    tape();
    quote({{"sequence", 1}});
    quote({{"sequence", 3}});
    CHECK(engine->tick().empty());
    quote({{"sequence", 4}});
    CHECK(engine->tick().empty());
    quote({{"sequence", 10}, {"reset_sequence", true}, {"source_time", wall() - 10}});
    CHECK(engine->tick().empty());
    quote({{"sequence", 11}});
    CHECK(child()["type"] == "submit");
}

TEST_CASE_FIXTURE(EngineFixture, "tape gap requires explicit recovery") {
    start();
    tape("500", "one", {{"sequence", 1}});
    tape("500", "three", {{"sequence", 3}});
    CHECK(engine->tick().empty());
    CHECK(Dec(status()["earned"]) == Dec(50));
    engine->set_feed_health("XYZ", true, "", true);
    tape("500", "four", {{"sequence", 4}});
    CHECK(Dec(child()["quantity"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "deadline and disconnect keep unknown commitment") {
    start({{"horizon_seconds", 30}});
    tape();
    json c = child();
    report(c, "NEW");
    engine->set_feed_health("XYZ", false);
    CHECK(action_types(engine->tick()) == std::vector<std::string>{"cancel"});
    now = 31;
    quote();
    CHECK(action_types(engine->tick()) == std::vector<std::string>{"query"});
    json result = status();
    CHECK(result["state"] == "STOPPING");
    CHECK(result["residual_provisional"] == true);
    CHECK(Dec(result["reserved"]) == Dec(100));
    report(c, "CANCELED", "30", {{"final", true}});
    CHECK(status()["state"] == "DEADLINE");
    CHECK(status()["residual_provisional"] == false);
}

TEST_CASE_FIXTURE(EngineFixture, "restart reconciles without replaying submit") {
    start();
    tape();
    json c = child();
    engine->close();
    now = 20;
    reopen();
    quote();
    std::vector<json> actions = engine->tick();
    CHECK(action_types(actions) == std::vector<std::string>{"query"});
    CHECK(actions[0]["client_order_id"] == c["client_order_id"]);
    CHECK(Dec(status()["reserved"]) == Dec(100));
    report(c, "NEW");
    CHECK(action_types(engine->drain_actions()) == std::vector<std::string>{"cancel"});
}

TEST_CASE_FIXTURE(EngineFixture, "stop does not refund without terminal") {
    start();
    tape();
    json c = child();
    json stopped = engine->request_stop(parent);
    CHECK(stopped["state"] == "STOPPING");
    CHECK(Dec(stopped["reserved"]) == Dec(100));
    report(c, "NEW");
    CHECK(action_types(engine->drain_actions()) == std::vector<std::string>{"cancel"});
    report(c, "CANCELED", "10", {{"final", true}});
    CHECK(status()["state"] == "STOPPED");
}

TEST_CASE_FIXTURE(EngineFixture, "failed cancel retried only after reconciled working and throttle") {
    start();
    tape();
    json c = child();
    report(c, "NEW", "0", {{"sequence", 1}});
    now = 2;
    CHECK(action_types(engine->tick()) == std::vector<std::string>{"cancel"});
    engine->on_action_error(S(c["client_order_id"]), "cancel", "timeout");
    engine->drain_actions();
    now = 4;
    report(c, "NEW", "0", {{"sequence", 2}});
    CHECK(engine->drain_actions().empty());
    now = 7;
    engine->tick();
    report(c, "NEW", "0", {{"sequence", 3}});
    CHECK(action_types(engine->drain_actions()) == std::vector<std::string>{"cancel"});
    CHECK(Dec(status()["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "capacity counts credit and reservation with stress scenarios") {
    start();
    tape("2000");
    child();
    engine->set_remaining_volume_forecast(parent, Dec("10000"));
    engine->tick();
    json capacity = status()["capacity"];
    CHECK(Dec(capacity["unfunded"]) == Dec(800));
    CHECK(capacity["scenarios"]["base"]["credit_shortfall"] == false);
    CHECK(capacity["scenarios"]["75_percent"]["credit_shortfall"] == true);
    CHECK(capacity["scenarios"]["50_percent"]["credit_shortfall"] == true);
}

TEST_CASE_FIXTURE(EngineFixture, "json safe status and durable event log") {
    start();
    tape();
    child();
    CHECK_NOTHROW(json(engine->list_parents()).dump());
    std::vector<json> events = engine->events(parent);
    bool submit_intent = false, tape_credit = false;
    for (auto& e : events) {
        if (e["kind"] == "submit_intent") submit_intent = true;
        if (e["kind"] == "tape_credit") tape_credit = true;
    }
    CHECK(submit_intent);
    CHECK(tape_credit);
}

TEST_CASE_FIXTURE(EngineFixture, "minimum notional increases minimum legal child cap") {
    start({{"quantity", "10"}, {"horizon_seconds", 7200}, {"min_notional", "100"}});
    CHECK(Dec(status()["child_cap"]) == Dec(10));
    tape("100");
    CHECK(Dec(child()["quantity"]) == Dec(10));
}

TEST_CASE_FIXTURE(EngineFixture, "known bid consumption survives repeated old snapshot") {
    start();
    tape("2000");
    json first = child();
    report(first, "FILLED", "100", {{"final", true}});
    now = 5;
    quote({{"source_time", wall_start}});
    CHECK(Dec(child()["quantity"]) == Dec(90));
}

TEST_CASE_FIXTURE(EngineFixture, "new source snapshot reconciles known consumption") {
    start();
    tape("2000");
    json first = child();
    report(first, "FILLED", "100", {{"final", true}});
    now = 5;
    quote();
    CHECK(Dec(child()["quantity"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "broker rejection stops parent without retry") {
    start();
    tape();
    json c = child();
    report(c, "REJECTED", "0", {{"final", true}});
    now = 10;
    quote();
    CHECK(engine->tick().empty());
    CHECK(status()["state"] == "STOPPED");
    CHECK(Dec(status()["free_credit"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "persisted conflict blocks a fresh parent after stop") {
    start();
    tape();
    json c = child();
    report(c, "CANCELED", "10", {{"sequence", 1}, {"final", true}});
    report(c, "CANCELED", "20", {{"sequence", 2}, {"final", true}});
    engine->request_stop(parent);
    CHECK_THROWS_WITH(start(), "unresolved Smart selling ledger conflict requires reconciliation");
}

TEST_CASE_FIXTURE(EngineFixture, "historical child context and broker lookup") {
    start();
    tape();
    json c = child();
    report(c, "CANCELED", "20", {{"sequence", 1}, {"final", true}});
    auto context = engine->get_order_context(S(c["client_order_id"]));
    REQUIRE(context);
    CHECK((*context)["active"] == false);
    CHECK((*context)["state"] == "TERMINAL");
    CHECK((*context)["cumulative_filled"] == "20");
    auto found = engine->find_order_by_broker_id("A", "broker-" + c["client_order_id"].get<std::string>());
    REQUIRE(found);
    CHECK(*found == *context);
    CHECK_FALSE(engine->find_order_by_broker_id("B", "broker-" + c["client_order_id"].get<std::string>()));
}

TEST_CASE_FIXTURE(EngineFixture, "submit action contains original send time and deadline") {
    json p = start();
    tape();
    json c = child();
    CHECK(c["send_timestamp"].get<double>() == wall());
    CHECK(c["deadline"] == p["deadline"]);
    CHECK(c["send_time"] == p["start"]);
}

TEST_CASE_FIXTURE(EngineFixture, "latency metrics report count median and observed max") {
    start();
    tape();
    json c = child();
    now = 0.6;
    report(c, "NEW");
    json metrics = status()["latencies"]["send_to_ack"];
    CHECK(metrics["count"] == 1);
    CHECK(metrics["median"].get<double>() == doctest::Approx(0.6));
    CHECK(metrics["observed_max"].get<double>() == doctest::Approx(0.6));
    CHECK_FALSE(metrics.contains("samples"));
}

TEST_CASE_FIXTURE(EngineFixture, "cumulative interval straddling parent start is not credited") {
    start();
    tape("1000", "trade-1", {{"interval_start", wall_start - 5}});
    CHECK(Dec(status()["earned"]) == Dec(0));
    now = 5;
    tape("1000", "next", {{"interval_start", wall_start}});
    CHECK(Dec(status()["earned"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "final dispatch shrinks to fresh depth and preserves accounting") {
    start();
    tape();
    json original = child();
    quote({{"bid_size", "400"}, {"bid", "10.02"}, {"ask", "10.03"}});
    auto updated = engine->prepare_dispatch(S(original["client_order_id"]));
    REQUIRE(updated);
    CHECK(Dec((*updated)["quantity"]) == Dec(40));
    CHECK(Dec((*updated)["limit_price"]) == Dec("10.02"));
    CHECK((*updated)["send_time"] == original["send_time"]);
    CHECK(Dec(status()["free_credit"]) == Dec(60));
    CHECK(Dec(status()["reserved"]) == Dec(40));
    CHECK_FALSE(engine->prepare_dispatch(S(original["client_order_id"])));
}

TEST_CASE_FIXTURE(EngineFixture, "final dispatch does not enlarge child on deeper book") {
    start();
    tape();
    quote({{"bid_size", "400"}});
    json original = child();
    quote({{"bid_size", "10000"}});
    auto updated = engine->prepare_dispatch(S(original["client_order_id"]));
    REQUIRE(updated);
    CHECK(Dec((*updated)["quantity"]) == Dec(40));
}

TEST_CASE_FIXTURE(EngineFixture, "halt before local dispatch releases unhanded reservation") {
    start({{"mode", "patient"}});
    tape("4000");
    json original = child();
    engine->set_feed_health("XYZ", false, "halted");
    CHECK_FALSE(engine->prepare_dispatch(S(original["client_order_id"])));
    json s = status();
    CHECK(Dec(s["reserved"]) == Dec(0));
    CHECK(Dec(s["free_credit"]) == Dec(120));
    CHECK(s["fallback"] == false);
    CHECK(s["state"] == "HOLD");
    CHECK((*engine->get_order_context(S(original["client_order_id"])))["state"] == "TERMINAL");
}

TEST_CASE_FIXTURE(EngineFixture, "stop before dispatch releases without broker cancel proof") {
    start();
    tape();
    json original = child();
    engine->request_stop(parent);
    CHECK_FALSE(engine->prepare_dispatch(S(original["client_order_id"])));
    CHECK(status()["state"] == "STOPPED");
    CHECK(Dec(status()["reserved"]) == Dec(0));
}

TEST_CASE_FIXTURE(EngineFixture, "deadline before dispatch never hands off order") {
    start({{"horizon_seconds", 1}});
    tape();
    json original = child();
    now = 2;
    quote();
    CHECK_FALSE(engine->prepare_dispatch(S(original["client_order_id"])));
    CHECK(status()["state"] == "DEADLINE");
    CHECK(Dec(status()["reserved"]) == Dec(0));
}

TEST_CASE_FIXTURE(EngineFixture, "pre-dispatch shrink below notional minimum releases unsent") {
    start({{"min_notional", "500"}});
    tape();
    json original = child();
    quote({{"bid_size", "400"}});
    CHECK_FALSE(engine->prepare_dispatch(S(original["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec(0));
    CHECK(status()["reason"].get<std::string>().find("minimum") != std::string::npos);
}

TEST_CASE_FIXTURE(EngineFixture, "pre-dispatch gate never releases acknowledged or prepared order") {
    start();
    tape();
    json original = child();
    report(original, "NEW");
    engine->set_feed_health("XYZ", false);
    CHECK_FALSE(engine->prepare_dispatch(S(original["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "long dispatch queue aborts unsent child without changing fallback") {
    start();
    tape();
    json original = child();
    now = 2;
    CHECK_FALSE(engine->prepare_dispatch(S(original["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec(0));
    CHECK(status()["reason"].get<std::string>().find("queue") != std::string::npos);
}

TEST_CASE_FIXTURE(EngineFixture, "market watermark persists across restart") {
    json checkpoint = {{"session", "2026-10-22"}, {"cumulative", "12345"}, {"observed_at", 1800000000}};
    engine->set_market_checkpoint("xyz", checkpoint);
    engine->close();
    reopen();
    CHECK(engine->get_market_checkpoint("XYZ") == checkpoint);
    CHECK_FALSE(engine->get_market_checkpoint("OTHER"));
}

TEST_CASE_FIXTURE(EngineFixture, "depth heartbeat does not replenish known bid consumption") {
    start();
    tape("2000");
    json c = child();
    report(c, "FILLED", "100", {{"final", true}});
    now = 5;
    quote({{"preserve_consumption", true}});
    CHECK(Dec(child()["quantity"]) == Dec(90));
}

TEST_CASE_FIXTURE(EngineFixture, "abort prepared but unpublished child preserves fallback") {
    start({{"mode", "patient"}});
    tape("10000");
    json first = child();
    report(first, "CANCELED", "0", {{"final", true}});
    now = 5;
    quote();
    json c = child();
    CHECK(status()["fallback"] == true);
    CHECK(engine->prepare_dispatch(S(c["client_order_id"])));
    CHECK(engine->abort_dispatch(S(c["client_order_id"]), "local position check failed"));
    CHECK(Dec(status()["reserved"]) == Dec(0));
    CHECK(status()["fallback"] == true);
    CHECK_FALSE(engine->abort_dispatch(S(c["client_order_id"]), "duplicate"));
}

TEST_CASE_FIXTURE(EngineFixture, "abort known unsent can stop parent") {
    start();
    tape();
    json c = child();
    CHECK(engine->abort_dispatch(S(c["client_order_id"]), "local holdings insufficient", true));
    CHECK(status()["state"] == "STOPPED");
    CHECK(Dec(status()["reserved"]) == Dec(0));
}

TEST_CASE_FIXTURE(EngineFixture, "abort never refunds an ambiguous or acknowledged order") {
    start();
    tape();
    json c = child();
    engine->on_action_error(S(c["client_order_id"]), "submit", "unknown delivery");
    CHECK_FALSE(engine->abort_dispatch(S(c["client_order_id"]), "not proof"));
    CHECK(Dec(status()["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "feedback metadata conflict holds without releasing") {
    start();
    tape();
    json c = child();
    CHECK(engine->hold_order(S(c["client_order_id"]), "quantity mismatch"));
    CHECK(status()["state"] == "HOLD");
    CHECK(Dec(status()["reserved"]) == Dec(100));
    CHECK_FALSE(engine->hold_order("unknown", "conflict"));
}

TEST_CASE_FIXTURE(EngineFixture, "timer reconciliation releases only never prepared queued intent") {
    start();
    tape();
    json c = child();
    now = 4;
    engine->tick();
    CHECK(status()["order_state"] == "RECONCILE");
    CHECK_FALSE(engine->prepare_dispatch(S(c["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec(0));
    CHECK(Dec(status()["free_credit"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "prepared timer reconciliation keeps ambiguous reservation") {
    start();
    tape();
    json c = child();
    CHECK(engine->prepare_dispatch(S(c["client_order_id"])));
    now = 4;
    engine->tick();
    CHECK_FALSE(engine->prepare_dispatch(S(c["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "transport uncertainty never becomes a local queue abort") {
    start();
    tape();
    json c = child();
    engine->on_action_error(S(c["client_order_id"]), "submit", "ambiguous delivery");
    now = 4;
    engine->tick();
    CHECK_FALSE(engine->prepare_dispatch(S(c["client_order_id"])));
    CHECK(Dec(status()["reserved"]) == Dec(100));
}

TEST_CASE_FIXTURE(EngineFixture, "external decimal bounds do not truncate derived fractional credit") {
    for (const char* value : {"1e25", "1e-25", "123456789012345678901234.1234567"}) {
        INFO(value);
        CHECK_THROWS_WITH_AS(ng::smart::parse_input_decimal(json(value)), doctest::Contains("precision"), ng::smart::ValueError);
    }
    start({{"mode", "patient"}, {"quantity", "1"}, {"quantity_step", "1e-24"}, {"min_quantity", "1e-24"}});
    tape("1e-24");
    CHECK(Dec(status()["free_credit"]) == Dec("3e-26"));
    CHECK_THROWS_WITH_AS(tape("1e-25", "out-of-bounds"), doctest::Contains("precision"), ng::smart::ValueError);
}

TEST_CASE_FIXTURE(EngineFixture, "derived volume forecast preserves fractional rate precision") {
    start();
    Decimal one_share_per_minute = Decimal(1).div(Decimal(60), 30);
    for (Decimal remaining : {Decimal(7200), Decimal(300)}) {
        Decimal forecast = one_share_per_minute * remaining;
        engine->set_remaining_volume_forecast(parent, forecast);
        engine->tick();
        json capacity = status()["capacity"];
        CHECK(capacity["forecast_available"] == true);
        CHECK(Dec(capacity["scenarios"]["base"]["eligible_volume"]) == forecast);
    }
    CHECK_THROWS(engine->set_remaining_volume_forecast(parent, Decimal(-1)));
}

TEST_CASE_FIXTURE(EngineFixture, "newer working report after terminal quarantines the current child") {
    start();
    tape("2000");
    json archived = child();
    report(archived, "CANCELED", "0", {{"sequence", 2}, {"final", true}});
    now = 5;
    quote();
    json current = child();
    report(current, "NEW", "0", {{"sequence", 1}});
    report(archived, "NEW", "0", {{"sequence", 1}});
    CHECK(status()["order_state"] == "LIVE");
    CHECK(status()["state"] != "HOLD");
    report(archived, "NEW", "0", {{"sequence", 3}});
    json s = status();
    CHECK(s["state"] == "HOLD");
    CHECK(s["order_state"] == "RECONCILE");
    CHECK(s["active_order"]["client_order_id"] == current["client_order_id"]);
    CHECK(Dec(s["reserved"]) == Dec(100));
    CHECK(Dec(s["filled"]) == Dec(0));
    CHECK(s["reason"].get<std::string>().find("working after terminal") != std::string::npos);
    std::vector<json> actions = engine->tick();
    bool cancel_current = false;
    for (auto& a : actions) if (a["type"] == "cancel" && a["client_order_id"] == current["client_order_id"]) cancel_current = true;
    CHECK(cancel_current);
}

TEST_CASE_FIXTURE(EngineFixture, "round robin uses one shared slice and participation budget") {
    start({{"account_ids", json::array({"A", "B", "C"})}, {"account_quantities", {{"A", "400"}, {"B", "400"}, {"C", "400"}}}});
    tape("2000");
    json first = child();
    CHECK(first["account_id"] == "A");
    CHECK(Dec(first["quantity"]) == Dec("100"));
    CHECK(Dec(status()["earned"]) == Dec("200.00"));
    CHECK(engine->tick().empty());
    int index = 1;
    for (const char* expected : {"B", "C", "A"}) {
        report(first, "FILLED", first["quantity"].get<std::string>(), {{"final", true}});
        now = 5 * index;
        quote();
        tape("1000", "rotation-" + std::to_string(index));
        first = child();
        CHECK(first["account_id"] == expected);
        CHECK(Dec(first["quantity"]) == Dec(100));
        status();
        ++index;
    }
}

TEST_CASE_FIXTURE(EngineFixture, "short inventory spills only remainder and retires empty accounts") {
    start({{"account_ids", json::array({"empty", "A", "B", "C"})},
           {"account_quantities", {{"empty", "0"}, {"A", "40"}, {"B", "30"}, {"C", "930"}}}});
    tape("2000");
    struct Step { double now; const char* account; const char* quantity; };
    json s;
    for (Step step : {Step{0, "A", "40"}, Step{5, "B", "30"}, Step{10, "C", "30"}, Step{15, "C", "100"}}) {
        now = step.now;
        quote();
        json c = child();
        CHECK(c["account_id"] == step.account);
        CHECK(Dec(c["quantity"]) == Dec(step.quantity));
        if (step.now == 0) {
            CHECK(Dec(status()["accounts"]["A"]["reserved"]) == Dec("40"));
            now = 1;
            CHECK(engine->tick().empty());
        }
        report(c, "FILLED", c["quantity"].get<std::string>(), {{"final", true}});
        s = status();
    }
    CHECK(s["accounts"]["empty"]["done"] == true);
    CHECK(s["accounts"]["A"]["done"] == true);
    CHECK(s["accounts"]["B"]["done"] == true);
    CHECK(Dec(s["accounts"]["C"]["filled"]) == Dec("130"));
}

TEST_CASE_FIXTURE(EngineFixture, "split slice keeps remainder when fresh depth is smaller") {
    start({{"account_ids", json::array({"A", "B", "C"})}, {"account_quantities", {{"A", "40"}, {"B", "500"}, {"C", "460"}}}});
    tape("2000");
    json first = child();
    report(first, "FILLED", "40", {{"final", true}});
    now = 5;
    quote({{"bid_size", "500"}});
    json second = child();
    CHECK(second["account_id"] == "B");
    CHECK(Dec(second["quantity"]) == Dec("50"));
    report(second, "FILLED", "50", {{"final", true}});
    now = 10;
    quote();
    json third = child();
    CHECK(third["account_id"] == "C");
    CHECK(Dec(third["quantity"]) == Dec("10"));
}

TEST_CASE_FIXTURE(EngineFixture, "partial fill cancel refunds only selected account and rotates") {
    start({{"account_ids", json::array({"A", "B"})}, {"account_quantities", {{"A", "500"}, {"B", "500"}}}});
    tape("2000");
    json first = child();
    report(first, "PARTIALLY_FILLED", "30");
    now = 2;
    std::vector<json> actions = engine->tick();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0]["type"] == "cancel");
    CHECK(actions[0]["account_id"] == "A");
    json before = status();
    CHECK(Dec(before["accounts"]["A"]["reserved"]) == Dec("70"));
    CHECK(Dec(before["accounts"]["B"]["reserved"]) == Dec("0"));
    report(first, "CANCELED", "40", {{"final", true}});
    json after = status();
    CHECK(Dec(after["accounts"]["A"]["filled"]) == Dec("40"));
    CHECK(Dec(after["accounts"]["A"]["reserved"]) == Dec("0"));
    now = 5;
    quote();
    json second = child();
    CHECK(second["account_id"] == "B");
    report(second, "NEW");
    auto context = engine->get_order_context(S(second["client_order_id"]));
    REQUIRE(context);
    CHECK((*context)["account_id"] == "B");
    CHECK(*engine->find_order_by_broker_id("B", "broker-" + second["client_order_id"].get<std::string>()) == *context);
    CHECK_FALSE(engine->find_order_by_broker_id("A", "broker-" + second["client_order_id"].get<std::string>()));
}

TEST_CASE_FIXTURE(EngineFixture, "fractional fill never creates an off step account order") {
    start({{"account_ids", json::array({"A", "B"})}, {"account_quantities", {{"A", "100"}, {"B", "900"}}}});
    tape("3000");
    json first = child();
    report(first, "CANCELED", "20.5", {{"final", true}});
    now = 5;
    quote();
    json second = child();
    CHECK(second["account_id"] == "B");
    report(second, "FILLED", second["quantity"].get<std::string>(), {{"final", true}});
    now = 10;
    quote();
    tape("1000", "fractional-fill-followup");
    json third = child();
    CHECK(third["account_id"] == "A");
    CHECK(Dec(third["quantity"]) == Dec("79"));
    json account = status()["accounts"]["A"];
    CHECK(Dec(account["remaining"]) == Dec("79.5"));
    CHECK(Dec(account["reserved"]) == Dec(79));
}

TEST_CASE_FIXTURE(EngineFixture, "multi account pre-dispatch refund keeps account reservations exact") {
    start({{"account_ids", json::array({"A", "B"})}, {"account_quantities", {{"A", "500"}, {"B", "500"}}}});
    tape("2000");
    json first = child();
    quote({{"bid_size", "400"}});
    auto prepared = engine->prepare_dispatch(S(first["client_order_id"]));
    REQUIRE(prepared);
    CHECK((*prepared)["account_id"] == "A");
    CHECK(Dec(status()["accounts"]["A"]["reserved"]) == Dec("40"));
    CHECK(engine->abort_dispatch(S(first["client_order_id"]), "not published"));
    CHECK(Dec(status()["accounts"]["A"]["reserved"]) == Dec("0"));
}

TEST_CASE_FIXTURE(EngineFixture, "round robin and inventory survive restart") {
    start({{"account_ids", json::array({"A", "B"})}, {"account_quantities", {{"A", "40"}, {"B", "960"}}}});
    tape("2000");
    json first = child();
    report(first, "FILLED", "40", {{"final", true}});
    engine->close();
    now = 5;
    reopen();
    quote();
    json second = child();
    CHECK(second["account_id"] == "B");
    CHECK(Dec(second["quantity"]) == Dec("60"));
    CHECK(status()["accounts"]["A"]["done"] == true);
}

TEST_CASE_FIXTURE(EngineFixture, "explicit target remains supported without exceeding inventory") {
    start({{"quantity", "150"}, {"horizon_seconds", 30}, {"account_ids", json::array({"A", "B"})},
           {"account_quantities", {{"A", "40"}, {"B", "960"}}}});
    tape("2000");
    struct Step { double now; const char* account; const char* quantity; };
    for (Step step : {Step{0, "A", "40"}, Step{5, "B", "60"}, Step{10, "B", "50"}}) {
        now = step.now;
        quote();
        json c = child();
        CHECK(c["account_id"] == step.account);
        CHECK(Dec(c["quantity"]) == Dec(step.quantity));
        report(c, "FILLED", step.quantity, {{"final", true}});
    }
    json s = status();
    CHECK(s["state"] == "COMPLETED");
    CHECK(Dec(s["filled"]) == Dec("150"));
    CHECK(Dec(s["accounts"]["B"]["remaining"]) == Dec("850"));
    CHECK(engine->tick().empty());
}

TEST_CASE_FIXTURE(EngineFixture, "selected account ownership and inventory are validated") {
    start({{"account_ids", json::array({"A", "B"})}, {"account_quantities", {{"A", "500"}, {"B", "500"}}}});
    CHECK_THROWS_WITH(start({{"account_id", "B"}}), "an active Smart selling parent already owns this account/instrument");
    CHECK_THROWS_WITH(start({{"account_ids", json::array({"C", "D"})}, {"account_quantities", {{"C", "40"}, {"D", "50"}}}}),
                      "quantity exceeds the selected accounts' available inventory");
    CHECK_THROWS_WITH(start({{"account_ids", json::array({"C", "D"})}, {"account_quantities", {{"C", "-1"}, {"D", "1001"}}}}),
                      "invalid account quantity");
}

TEST_CASE_FIXTURE(EngineFixture, "no deadline runs past sizing horizon and survives restart") {
    json p = start({{"deadline", nullptr}, {"start_time", "now"}});
    CHECK(p["deadline"].is_null());
    engine->close();
    now = 10000;
    reopen();
    quote();
    tape();
    json c = child();
    CHECK(c["deadline"].is_null());
    CHECK(status()["active"] == true);
    CHECK(status()["state"] == "RUNNING");
}

TEST_CASE_FIXTURE(EngineFixture, "no deadline does not create schedule pressure") {
    start({{"mode", "patient"}, {"deadline", nullptr}});
    now = 10000;
    quote({{"bid", "10"}, {"ask", "10.10"}});
    tape("4000");
    CHECK(Dec(child()["limit_price"]) == Dec("10.10"));
}

TEST_CASE_FIXTURE(EngineFixture, "legacy parent is upgraded without losing reserved inventory") {
    start();
    tape();
    json c = child();
    {
        ng::Sqlite db(path);
        ng::Statement select(db, "SELECT data FROM smart_parents WHERE id=?");
        select.bind(1, parent);
        REQUIRE(select.step());
        json legacy = json::parse(select.column_text(0));
        for (const char* key : {"accounts", "account_ids", "next_account_index", "slice_remaining"}) legacy.erase(key);
        legacy["order"].erase("account_id");
        ng::Statement update(db, "UPDATE smart_parents SET data=? WHERE id=?");
        update.bind(1, legacy.dump());
        update.bind(2, parent);
        update.exec();
    }
    engine->close();
    reopen();
    json s = status();
    CHECK(s["account_ids"] == json::array({"A"}));
    CHECK(Dec(s["accounts"]["A"]["reserved"]) == Dec("100"));
    CHECK(s["active_order"]["account_id"] == "A");
    report(c, "CANCELED", "20", {{"final", true}});
    CHECK(Dec(status()["accounts"]["A"]["filled"]) == Dec("20"));
}

TEST_CASE_FIXTURE(EngineFixture, "defaults and required broker rules") {
    json p = start({{"mode", "2"}, {"horizon_seconds", 7200}});
    CHECK(p["mode"] == "normal");
    CHECK_THROWS(engine->start_parent(json{{"account_id", "B"}, {"symbol", "XYZ"}, {"quantity", "100"}}));
    CHECK_THROWS(start({{"account_id", "B"}, {"quantity", "100.5"}}));
}

TEST_CASE_FIXTURE(EngineFixture, "python-written journal values are read back exactly") {
    // Parent JSON as the Python engine serialises it (floats for clocks, strings for decimals).
    start();
    tape();
    json c = child();
    report(c, "PARTIALLY_FILLED", "40", {{"sequence", 1}});
    engine->close();
    {
        ng::Sqlite db(path);
        ng::Statement select(db, "SELECT data FROM smart_parents WHERE id=?");
        select.bind(1, parent);
        REQUIRE(select.step());
        json stored = json::parse(select.column_text(0));
        CHECK(Dec(stored["F"]) == Dec("40"));
        CHECK(Dec(stored["R"]) == Dec("60"));
        CHECK(Dec(stored["participation"]) == Dec("0.10"));
        CHECK(Dec(stored["order"]["cumulative_filled"]) == Dec("40"));
    }
    reopen();
    json s = status();
    CHECK(Dec(s["filled"]) == Dec("40"));
    CHECK(Dec(s["reserved"]) == Dec("60"));
    CHECK(s["order_state"] == "RECONCILE");
}
