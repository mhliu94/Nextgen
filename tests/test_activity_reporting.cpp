// Durable order, tag and algorithm-session reports (tests/test_activity_reporting.py).
#include <doctest.h>

#include <atomic>
#include <thread>

#include "core/sqlite.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "orders/order_status.h"
#include "reporting/activity_reporting.h"
#include "test_helpers.h"

using ng::json;
using ng::reporting::ActivityReporting;

namespace {

double epoch(const char* iso) { return ng::parse_iso8601(iso)->epoch_seconds; }

ng::orders::OrderEvent order(std::initializer_list<std::pair<const char*, json>> updates = {}) {
    json payload = {{"schema_version", 1},
                    {"account_id", "A"},
                    {"order_id", "order-1"},
                    {"sequence", 0},
                    {"created_at", "2026-10-28T14:00:00Z"},
                    {"updated_at", "2026-10-28T14:00:00Z"},
                    {"symbol", "AAPL"},
                    {"side", "BUY"},
                    {"order_type", "MARKET"},
                    {"status", "PARTIALLY_FILLED"},
                    {"quantity", 10},
                    {"filled_quantity", 2},
                    {"average_fill_price", 100},
                    {"currency", "USD"}};
    for (const auto& [key, value] : updates) payload[key] = value;
    if (payload["currency"].is_null()) payload.erase("currency");
    return ng::orders::parse_order_message(payload.dump());
}

struct ReportingFixture {
    ngtest::TempDir dir{"activity"};
    double now = epoch("2026-10-28T14:00:00+00:00");
    ng::AccountDirectory accounts = ngtest::directory({ngtest::meta("A", 1, "Tiger"), ngtest::meta("B", 2, "Futu")});
    std::unique_ptr<ActivityReporting> reporting;

    ReportingFixture() { reporting = open(); }
    std::unique_ptr<ActivityReporting> open() {
        return std::make_unique<ActivityReporting>(dir.file("activity.sqlite3"), [this] { return now; });
    }
    json tag_report(const std::string& tag_id, ActivityReporting* service = nullptr) {
        json snapshot = (service ? service : reporting.get())->snapshot(accounts);
        for (const auto& tag : snapshot["tags"]) {
            if (tag["id"] == tag_id) return tag;
        }
        FAIL("tag not reported");
        return nullptr;
    }
    json session_report(const std::string& session_id = "session-1") {
        json snapshot = reporting->snapshot(accounts);
        for (const auto& item : snapshot["sessions"]) {
            if (item["id"] == session_id) return item;
        }
        FAIL("session not reported");
        return nullptr;
    }
};

}  // namespace

TEST_CASE_FIXTURE(ReportingFixture, "cumulative fills replace duplicates, reordering and corrections") {
    std::string tag = reporting->start_tag("Opening buys", "aapl");
    CHECK(reporting->record_order(order()));
    CHECK_FALSE(reporting->record_order(order()));
    CHECK(reporting->record_order(order({{"sequence", 2}, {"filled_quantity", 6}, {"average_fill_price", 110}})));
    CHECK_FALSE(reporting->record_order(order({{"sequence", 1}, {"filled_quantity", 4}})));
    json total = tag_report(tag)["totals"];
    CHECK(total["shares_bought"] == 6.0);
    CHECK(total["cash_by_currency"]["USD"]["buy_cost"] == 660.0);

    // A higher-sequence broker correction replaces cumulative totals, even downwards.
    CHECK(reporting->record_order(order({{"sequence", 3}, {"filled_quantity", 3}, {"average_fill_price", 105}})));
    total = tag_report(tag)["totals"];
    CHECK(total["shares_bought"] == 3.0);
    CHECK(total["cash_by_currency"]["USD"]["cash_change"] == -315.0);
}

TEST_CASE_FIXTURE(ReportingFixture, "tag boundaries, stop and restart preserve late fills") {
    std::string first = reporting->start_tag("First", "AAPL");
    reporting->record_order(order({{"order_id", "too-early"}, {"created_at", "2026-10-28T13:59:59Z"}}));
    reporting->record_order(order());
    now += 60;
    reporting->stop_tag(first);
    reporting->stop_tag(first);
    std::string second = reporting->start_tag("Second", "AAPL");
    ng::orders::OrderEvent boundary =
        order({{"order_id", "boundary"}, {"created_at", "2026-10-28T14:01:00Z"}, {"updated_at", "2026-10-28T14:01:00Z"}});
    reporting->record_order(boundary);
    // A fill reported later is still attached to its original order's tagging period.
    reporting->record_order(order({{"sequence", 1}, {"filled_quantity", 8}, {"updated_at", "2026-10-28T14:02:00Z"}}));
    CHECK(tag_report(first)["totals"]["shares_bought"] == 8.0);
    CHECK(tag_report(second)["totals"]["shares_bought"] == 2.0);
    CHECK(reporting->snapshot(accounts)["active_tag_id"] == second);
    auto tags = reporting->order_tags({order().payload, boundary.payload});
    CHECK(tags.size() == 2);
    CHECK(tags.at({"A", "order-1"}) == json::array({{{"id", first}, {"name", "First"}}}));
    CHECK(tags.at({"A", "boundary"}) == json::array({{{"id", second}, {"name", "Second"}}}));
    CHECK_THROWS_AS(reporting->stop_tag("missing"), ng::reporting::KeyError);
}

TEST_CASE_FIXTURE(ReportingFixture, "tag and order history survive restart and midnight") {
    now = epoch("2026-10-29T03:59:00+00:00");
    std::string tag = reporting->start_tag("Overnight", "AAPL");
    reporting->record_order(order({{"created_at", "2026-10-29T03:59:00Z"}, {"updated_at", "2026-10-29T03:59:00Z"}}));
    reporting->close();
    now += 120;
    reporting = open();
    reporting->record_order(order({{"sequence", 1}, {"filled_quantity", 5}, {"created_at", "2026-10-29T03:59:00Z"},
                                   {"updated_at", "2026-10-29T04:01:00Z"}}));
    CHECK(reporting->snapshot(accounts)["active_tag_id"] == tag);
    CHECK(tag_report(tag)["totals"]["shares_bought"] == 5.0);
    reporting->stop_tag(tag);
    CHECK(reporting->snapshot(accounts)["active_tag_id"].is_null());
}

TEST_CASE_FIXTURE(ReportingFixture, "tag summarizes buys, sells and only changed visible accounts") {
    std::string tag = reporting->start_tag("Both sides", "AAPL");
    reporting->record_order(order({{"filled_quantity", 3}, {"average_fill_price", 101}}));
    reporting->record_order(order({{"account_id", "B"}, {"side", "SELL"}, {"filled_quantity", 5}, {"average_fill_price", 110}}));
    reporting->record_order(order({{"account_id", "HIDDEN"}, {"filled_quantity", 9}}));
    reporting->record_order(order({{"order_id", "other-symbol"}, {"symbol", "MSFT"}, {"filled_quantity", 9}}));
    reporting->record_order(order({{"order_id", "unfilled"}, {"status", "NEW"}, {"filled_quantity", 0}, {"average_fill_price", nullptr}}));
    json report = tag_report(tag);
    REQUIRE(report["accounts"].size() == 2);
    CHECK(report["accounts"][0]["account_id"] == "A");
    CHECK(report["accounts"][1]["account_id"] == "B");
    CHECK(report["accounts"][0]["shares_bought"] == 3.0);
    CHECK(report["accounts"][0]["account_num_id"] == 1);
    CHECK(report["accounts"][0]["broker"] == "Tiger");
    CHECK(report["accounts"][1]["shares_sold"] == 5.0);
    CHECK(report["totals"]["cash_by_currency"]["USD"] ==
          json{{"buy_cost", 303.0}, {"sell_proceeds", 550.0}, {"cash_change", 247.0}, {"complete", true}});
    CHECK(report["active"] == true);
    CHECK(report["name"] == "Both sides");
    CHECK(report["symbol"] == "AAPL");
    CHECK(report["started_at"] == "2026-10-28T14:00:00Z");
    CHECK(report["stopped_at"].is_null());
}

TEST_CASE_FIXTURE(ReportingFixture, "missing fill price stays unknown until authoritative correction") {
    std::string tag = reporting->start_tag("Unknown cost", "AAPL");
    reporting->record_order(order({{"average_fill_price", nullptr}}));
    reporting->record_order(order({{"order_id", "sell"}, {"side", "SELL"}, {"filled_quantity", 1}, {"average_fill_price", 120}}));
    json total = tag_report(tag)["totals"];
    CHECK(total["shares_bought"] == 2.0);
    CHECK(total["cash_complete"] == false);
    CHECK(total["cash_by_currency"]["USD"] ==
          json{{"buy_cost", nullptr}, {"sell_proceeds", 120.0}, {"cash_change", nullptr}, {"complete", false}});
    reporting->record_order(order({{"sequence", 1}, {"average_fill_price", 100}}));
    total = tag_report(tag)["totals"];
    CHECK(total["cash_complete"] == true);
    CHECK(total["cash_by_currency"]["USD"]["cash_change"] == -80.0);
}

TEST_CASE_FIXTURE(ReportingFixture, "cash is partitioned by currency and missing currency is explicit") {
    std::string tag = reporting->start_tag("Currencies", "AAPL");
    reporting->record_order(order());
    reporting->record_order(order({{"order_id", "hkd"}, {"side", "SELL"}, {"currency", "HKD"}, {"average_fill_price", 800}}));
    reporting->record_order(order({{"order_id", "unknown"}, {"currency", nullptr}, {"average_fill_price", 50}}));
    json cash = tag_report(tag)["totals"]["cash_by_currency"];
    CHECK(cash.size() == 3);
    CHECK(cash["USD"]["cash_change"] == -200.0);
    CHECK(cash["HKD"]["cash_change"] == 1600.0);
    CHECK(cash["UNKNOWN"]["buy_cost"].is_null());
    CHECK(cash["UNKNOWN"]["cash_change"].is_null());
    CHECK(cash["UNKNOWN"]["complete"] == false);
    CHECK(tag_report(tag)["totals"]["cash_complete"] == false);
}

TEST_CASE_FIXTURE(ReportingFixture, "sessions use exact account command correlation and keep zero fill accounts") {
    json params = {{"symbol", "AAPL"}, {"trading_mode", "E"}, {"fast_trading_price_limit", 120}};
    reporting->start_session("session-1", params, {"A", "B", "HIDDEN"});
    reporting->register_command("session-1", json{{"account_id", "A"}, {"command_id", "child-1"}, {"client_order_id", "client-1"}});
    reporting->record_order(order({{"command_id", "child-1"}}));
    reporting->record_order(order({{"order_id", "manual"}, {"command_id", "unrelated"}, {"filled_quantity", 9}}));
    reporting->record_order(order({{"account_id", "B"}, {"command_id", "child-1"}, {"filled_quantity", 8}}));
    json report = session_report();
    CHECK(report["parameters"] == params);
    CHECK(report["account_ids"] == json::array({"A", "B"}));
    CHECK(report["running"] == true);
    CHECK(report["status"] == "running");
    CHECK(report["symbol"] == "AAPL");
    CHECK(report["started_at"] == "2026-10-28T14:00:00Z");
    CHECK(report["ended_at"].is_null());
    CHECK(report["reason"].is_null());
    CHECK(report["totals"]["shares_bought"] == 2.0);
    REQUIRE(report["accounts"].size() == 2);
    CHECK(report["accounts"][0]["account_id"] == "A");
    CHECK(report["accounts"][1]["account_id"] == "B");
    CHECK(report["accounts"][1]["shares_bought"] == 0.0);
    CHECK(report["accounts"][1]["cash_by_currency"] == json::object());
    // A session whose accounts are all hidden is not reported at all.
    reporting->start_session("hidden-only", params, {"HIDDEN"});
    CHECK(reporting->snapshot(accounts)["sessions"].size() == 1);
    // Starting the same session again never overwrites its record.
    reporting->start_session("session-1", json{{"symbol", "MSFT"}}, {"B"});
    CHECK(session_report()["parameters"] == params);
}

TEST_CASE_FIXTURE(ReportingFixture, "late command registration repairs client id correlation") {
    reporting->start_session("session-1", json{{"symbol", "AAPL"}}, {"A"});
    reporting->record_order(order({{"client_order_id", "late-client"}}));
    CHECK(session_report()["totals"]["shares_bought"] == 0.0);
    reporting->register_command("session-1", json{{"account_id", "A"}, {"command_id", "late-command"}, {"client_order_id", "late-client"}});
    CHECK(session_report()["totals"]["shares_bought"] == 2.0);
    // Publishers may omit optional correlation fields in later full snapshots.
    reporting->record_order(order({{"sequence", 1}, {"filled_quantity", 4}}));
    CHECK(session_report()["totals"]["shares_bought"] == 4.0);
}

TEST_CASE_FIXTURE(ReportingFixture, "tag labels accept lowercase RFC 3339 timestamps") {
    std::string tag = reporting->start_tag("Opening", "AAPL");
    ng::orders::OrderEvent event = order({{"created_at", "2026-10-28t14:00:00z"}, {"updated_at", "2026-10-28t14:00:00z"}});
    reporting->record_order(event);
    CHECK(reporting->order_tags({event.payload}).at({"A", "order-1"}) == json::array({{{"id", tag}, {"name", "Opening"}}}));
}

TEST_CASE_FIXTURE(ReportingFixture, "command correlation cannot move to another session") {
    reporting->start_session("session-1", json{{"symbol", "AAPL"}}, {"A"});
    json command = {{"account_id", "A"}, {"command_id", "child-1"}};
    reporting->register_command("session-1", command);
    CHECK_THROWS_WITH_AS(reporting->register_command("session-2", command),
                         "An order command cannot belong to two algorithm sessions.", std::invalid_argument);
    reporting->record_order(order({{"command_id", "child-1"}}));
    CHECK(session_report()["totals"]["shares_bought"] == 2.0);
    // Commands without an account or identifiers are ignored.
    reporting->register_command("session-2", json{{"command_id", "child-1"}});
    reporting->register_command("session-2", json{{"account_id", ""}, {"command_id", "child-1"}});
    reporting->register_command("session-2", json{{"account_id", "A"}});
}

TEST_CASE_FIXTURE(ReportingFixture, "stale session heartbeat is unknown and an ended session stays stopped") {
    reporting->start_session("session-1", json{{"symbol", "AAPL"}}, {"A"});
    now += 91;
    json report = session_report();
    CHECK(report["status"] == "unknown");
    CHECK(report["running"].is_null());
    reporting->heartbeat_session("session-1", "stopping");
    CHECK(session_report()["status"] == "stopping");
    reporting->end_session("session-1", "stopped", std::string("Manual stop"));
    now += 86400;
    reporting->heartbeat_session("session-1");
    report = session_report();
    CHECK(report["status"] == "stopped");
    CHECK(report["running"] == false);
    CHECK(report["reason"] == "Manual stop");
    CHECK(report["ended_at"] == "2026-10-28T14:01:31Z");
}

TEST_CASE_FIXTURE(ReportingFixture, "immutable order fields are enforced on newer sequences") {
    reporting->record_order(order());
    CHECK_THROWS_WITH_AS(reporting->record_order(order({{"sequence", 1}, {"created_at", "2026-10-28T13:59:00Z"}})),
                         "created_at is immutable for an order.", std::invalid_argument);
    reporting->record_order(order({{"sequence", 1}, {"updated_at", "2026-10-28T14:05:00Z"}}));
    CHECK_THROWS_WITH_AS(reporting->record_order(order({{"sequence", 2}, {"updated_at", "2026-10-28T14:04:00Z"}})),
                         "updated_at cannot decrease when sequence increases.", std::invalid_argument);
    CHECK_THROWS_WITH_AS(reporting->record_order(order({{"sequence", 2}, {"updated_at", "2026-10-28T14:06:00Z"}, {"side", "SELL"}})),
                         "Order symbol and side are immutable.", std::invalid_argument);
    CHECK_THROWS_WITH_AS(reporting->record_order(order({{"sequence", 2}, {"updated_at", "2026-10-28T14:06:00Z"}, {"symbol", "MSFT"}})),
                         "Order symbol and side are immutable.", std::invalid_argument);
    // Case and surrounding whitespace never change the instrument.
    CHECK(reporting->record_order(order({{"sequence", 2}, {"updated_at", "2026-10-28T14:06:00Z"}, {"symbol", "aapl"}})));
}

TEST_CASE_FIXTURE(ReportingFixture, "tag names and symbols are validated") {
    for (const char* name : {"", "   ", "\xC2\xA0"}) {
        CHECK_THROWS_WITH_AS(reporting->start_tag(name, "AAPL"), "Tag name must contain 1 to 100 characters.", std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(reporting->start_tag(std::string(101, 'x'), "AAPL"), "Tag name must contain 1 to 100 characters.",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(reporting->start_tag("bad\nname", "AAPL"), "Tag name cannot contain control characters.",
                         std::invalid_argument);
    for (const std::string& symbol : {std::string(), std::string("  "), std::string(65, 'X')}) {
        CHECK_THROWS_WITH_AS(reporting->start_tag("ok", symbol), "Symbol must contain 1 to 64 characters.", std::invalid_argument);
    }
    for (const char* symbol : {"A B", "A\tB", "A\xC2\xA0" "B", "A\xE3\x80\x80" "B"}) {
        CHECK_THROWS_WITH_AS(reporting->start_tag("ok", symbol), "Symbol cannot contain whitespace or control characters.",
                             std::invalid_argument);
    }
    // 100 multi-byte characters are still 100 characters; names are stored stripped.
    std::string wide;
    for (int i = 0; i < 100; ++i) wide += "\xE8\xB4\xAD";
    std::string tag = reporting->start_tag("  " + wide + "  ", " aapl ");
    json report = tag_report(tag);
    CHECK(report["name"] == wide);
    CHECK(report["symbol"] == "AAPL");
    CHECK_THROWS_WITH_AS(reporting->start_tag("Another", "MSFT"), "Stop the active tag before starting another.",
                         std::invalid_argument);
    CHECK(tag.size() == 32);
}

TEST_CASE_FIXTURE(ReportingFixture, "two instances cannot start competing active tags") {
    auto first = open();
    auto second = open();
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::optional<std::string> results[2];
    auto start = [&](ActivityReporting& service, const char* name, std::optional<std::string>& result) {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        try {
            result = service.start_tag(name, "AAPL");
        } catch (const std::invalid_argument&) {
        }
    };
    std::thread one(start, std::ref(*first), "One", std::ref(results[0]));
    std::thread two(start, std::ref(*second), "Two", std::ref(results[1]));
    while (ready.load() < 2) std::this_thread::yield();
    go = true;
    one.join();
    two.join();
    CHECK((results[0].has_value() ? 1 : 0) + (results[1].has_value() ? 1 : 0) == 1);
    CHECK(first->snapshot(accounts) == second->snapshot(accounts));
    std::string tag = first->snapshot(accounts)["active_tag_id"].get<std::string>();
    second->stop_tag(tag);
    CHECK(first->snapshot(accounts)["active_tag_id"].is_null());
}

TEST_CASE_FIXTURE(ReportingFixture, "timestamps keep microseconds like Python datetimes") {
    now = epoch("2026-10-28T14:00:00+00:00") + 0.2500003;  // sub-microsecond noise is dropped
    std::string tag = reporting->start_tag("Precise", "AAPL");
    reporting->start_session("session-1", json{{"symbol", "AAPL"}}, {"A"}, std::string("2026-10-28T09:59:59.5-04:00"));
    CHECK(tag_report(tag)["started_at"] == "2026-10-28T14:00:00.250000Z");
    CHECK(session_report()["started_at"] == "2026-10-28T13:59:59.500000Z");
    CHECK(session_report()["updated_at"] == "2026-10-28T14:00:00.250000Z");
    ng::Sqlite raw(dir.file("activity.sqlite3"), ng::Sqlite::Mode::ReadOnly);
    ng::Statement row(raw, "SELECT start_epoch FROM activity_tags");
    REQUIRE(row.step());
    CHECK(ng::python_float_repr(row.column_double(0)) == ng::python_float_repr(epoch("2026-10-28T14:00:00.25+00:00")));
}
