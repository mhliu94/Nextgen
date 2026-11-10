// Durable algorithm start/stop events with holdings (tests/test_operations_log.py).
#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/time_util.h"
#include "ops/operations_log.h"
#include "test_helpers.h"

using ng::json;

TEST_CASE("records a pretty algo start event with holdings") {
    ngtest::TempDir dir("operations-log");
    ng::AccountSnapshot snapshot;
    snapshot.account_id = "A";
    snapshot.account_num_id = 1;
    snapshot.cash = 1000;
    snapshot.cash_by_currency = {{"USD", 1000}, {"HKD", 50}};
    snapshot.positions.push_back(ng::Position{"AAPL", 10, 123.45, std::nullopt});
    snapshot.ts = "2026-07-08T01:02:00Z";
    snapshot.trading_enabled = true;
    double started_at = ng::parse_iso8601("2026-07-08T01:02:03+00:00")->epoch_seconds;
    ng::OperationsLog operations_log(dir.path.string(), started_at,
                                     [&] { return std::map<std::string, ng::AccountSnapshot>{{"A", snapshot}}; }, "unit-test");
    operations_log.record_algo_started(json{{"command_id", "algo-start-1"},
                                            {"trading_mode", "F"},
                                            {"symbol", "AAPL"},
                                            {"end_time_et", "2099-01-01T00:00:00Z"},
                                            {"fast_trading_total_target_position", 500}},
                                       std::string("F:AAPL"), std::nullopt);

    CHECK(std::filesystem::path(operations_log.path()).filename() == "operations_20260708T010203Z.log");
    std::ifstream in(operations_log.path());
    std::stringstream content;
    content << in.rdbuf();
    CHECK(content.str().find("\n      \"event\": \"algo_trading_started\"") != std::string::npos);

    json events = operations_log.read_document().at("events");
    REQUIRE(events.size() == 1);
    const json& event = events[0];
    CHECK(event["ui_server_started_at"] == "2026-07-08T01:02:03Z");
    CHECK(event["session"]["command_id"] == "algo-start-1");
    CHECK(event["session"]["session_id"] == "algo-start-1");
    CHECK(event["session"]["strategy_key"] == "F:AAPL");
    CHECK(event["session"]["fast_trading_total_target_position"] == 500);
    CHECK_FALSE(event["session"].contains("fast_trading_price_limit"));
    CHECK(event["holdings"]["source"] == "unit-test");
    CHECK(event["holdings"]["accounts"][0]["cash_by_currency"]["USD"] == 1000.0);
    CHECK(event["holdings"]["accounts"][0]["securities"][0]["symbol"] == "AAPL");

    operations_log.record_algo_ended(json{{"command_id", "algo-start-1"}, {"trading_mode", "F"}, {"symbol", "AAPL"}},
                                     std::string("F:AAPL"), std::string("algo-start-1"), std::string("Total target position reached"),
                                     3, 4);
    events = operations_log.read_document().at("events");
    REQUIRE(events.size() == 2);
    CHECK(events[1]["event"] == "algo_trading_ended");
    CHECK(events[1]["details"] == json{{"reason", "Total target position reached"}, {"cycles", 3}, {"published_commands", 4}});
}
