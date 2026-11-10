// Operator reconciliation utilities (tests/test_reconcile_prior_day.py and
// tests/test_smart_selling_reconciliation.py).
#include "algo/smart_fixtures.h"

#include <filesystem>
#include <fstream>

#include "algo/smart_buying/engine.h"
#include "algo/smart_selling/feedback.h"
#include "algo/smart_selling/feedback_store.h"
#include "ops/reconcile.h"

using namespace ngtest;
using ng::ops::reconcile_prior_day;
using ng::ops::reconcile_stopped_child;
using ng::ops::ReconcileSource;
using ng::smart::OrderFeedbackStore;
using ng::smart::SmartSellingFeedback;

namespace {

const double NOW = ng::parse_iso8601("2026-11-06T03:00:00+00:00")->epoch_seconds;

ng::smart::OrderEvent snapshot(std::initializer_list<std::pair<const char*, json>> changes = {}) {
    json payload = {{"schema_version", 1}, {"account_id", "A"}, {"order_id", "old-order"}, {"symbol", "AEHL"}, {"side", "SELL"},
                    {"order_type", "LIMIT"}, {"status", "PENDING_NEW"}, {"sequence", 1}, {"quantity", 100},
                    {"filled_quantity", 0}, {"average_fill_price", nullptr}, {"limit_price", 10},
                    {"created_at", "2026-10-28T00:11:20Z"}, {"updated_at", "2026-10-28T00:11:20Z"}};
    for (const auto& [key, value] : changes) payload[key] = value;
    return ng::smart::parse_order_message(payload.dump());
}

long long scalar(const std::string& path, const std::string& sql) {
    ng::Sqlite db(path, ng::Sqlite::Mode::ReadOnly);
    ng::Statement row(db, sql);
    REQUIRE(row.step());
    return row.column_int(0);
}

std::vector<std::string> column(const std::string& path, const std::string& sql) {
    ng::Sqlite db(path, ng::Sqlite::Mode::ReadOnly);
    ng::Statement rows(db, sql);
    std::vector<std::string> out;
    while (rows.step()) out.push_back(rows.column_text(0));
    return out;
}

json read_json(const std::filesystem::path& path) {
    std::ifstream in(path);
    return json::parse(in);
}

struct PriorDayFixture {
    TempDir dir{"reconcile-prior-day"};
    std::vector<ReconcileSource> sources;
    std::vector<std::unique_ptr<OrderFeedbackStore>> feedback;

    PriorDayFixture() {
        for (const char* mode : {"G", "H"}) {
            std::string state = dir.file(std::string(mode) + ".sqlite3");
            {
                ng::Sqlite db(state);
                db.exec("CREATE TABLE smart_parents (id TEXT,active INTEGER,data TEXT)");
            }
            sources.push_back({mode, state});
            feedback.push_back(std::make_unique<OrderFeedbackStore>(feedback_path(state)));
        }
    }
    static std::string feedback_path(const std::string& state) { return state + ".feedback.sqlite3"; }
    json reconcile(bool apply = false, const std::optional<std::string>& backup = std::nullopt,
                   const std::vector<ReconcileSource>* use = nullptr) {
        return reconcile_prior_day(use ? *use : sources, "America/New_York", "Requested prior-day cleanup", backup, apply, NOW);
    }
};

}  // namespace

TEST_CASE_FIXTURE(PriorDayFixture, "prior-day preview, current day boundary, apply, backups and idempotency") {
    std::vector<std::string> ids = {"cmd_2026-11-05T01:00:00Z_old", "cmd_2026-11-06T01:00:00Z_current", "undated",
                                    "cmd_2026-11-07T01:00:00Z_future"};
    for (auto& store : feedback) {
        for (const auto& identity : ids) store->track_external_order(json{{"account_id", "A"}, {"symbol", "POM"}, {"command_id", identity}});
        store->record_snapshot(snapshot());
        store->record_snapshot(snapshot({{"order_id", "current-order"}, {"created_at", "2026-11-05T15:00:00Z"},
                                         {"updated_at", "2026-11-05T15:00:00Z"}}));
    }
    json preview = reconcile();
    CHECK(preview["trading_day"] == "2026-11-05");
    CHECK(preview["applied"] == false);
    CHECK(preview["as_of"] == "2026-11-06T03:00:00+00:00");
    CHECK(preview["timezone"] == "America/New_York");
    CHECK(preview["reason"] == "Requested prior-day cleanup");
    REQUIRE(preview["stores"].size() == 2);
    for (const auto& plan : preview["stores"]) {
        CHECK(plan["commands"].size() == 1);
        CHECK(plan["orders"].size() == 1);
        CHECK(plan["undated_commands"].size() == 1);
        CHECK(plan["orders"][0]["order_id"] == "old-order");
        CHECK(plan["commands"][0]["command_id"] == ids[0]);
    }
    CHECK(preview["stores"][0]["mode"] == "G");
    CHECK(preview["stores"][1]["mode"] == "H");
    for (const auto& source : sources) CHECK(scalar(feedback_path(source.state_db), "SELECT COUNT(*) FROM feedback_orders") == 2);

    std::filesystem::path backup = dir.path / "backup";
    json result = reconcile(true, backup.string());
    CHECK(result["applied"] == true);
    int copies = 0;
    for (const auto& entry : std::filesystem::directory_iterator(backup)) copies += entry.path().extension() == ".sqlite3" ? 1 : 0;
    CHECK(copies == 4);
    CHECK(read_json(backup / "preview.json")["applied"] == false);
    CHECK(read_json(backup / "result.json")["applied"] == true);
    // The backups hold the state before the cleanup.
    CHECK(scalar((backup / "2-G.sqlite3.feedback.sqlite3").string(), "SELECT COUNT(*) FROM feedback_orders") == 2);
    for (const auto& source : sources) {
        std::string path = feedback_path(source.state_db);
        CHECK(column(path, "SELECT pending FROM feedback_pending_commands WHERE command_id='" + ids[0] + "'") == std::vector<std::string>{"0"});
        std::vector<std::string> still_pending(ids.begin() + 1, ids.end());
        std::sort(still_pending.begin(), still_pending.end());
        CHECK(column(path, "SELECT command_id FROM feedback_pending_commands WHERE pending=1 ORDER BY command_id") == still_pending);
        CHECK(column(path, "SELECT order_id FROM feedback_orders") == std::vector<std::string>{"current-order"});
        auto archived = column(path, "SELECT previous_data FROM feedback_reconciled_orders");
        REQUIRE(archived.size() == 1);
        CHECK(json::parse(archived[0])["status"] == "PENDING_NEW");
        CHECK(column(path, "SELECT reason FROM feedback_reconciled_orders") == std::vector<std::string>{"Requested prior-day cleanup"});
        CHECK(column(path, "SELECT reconciled_at FROM feedback_reconciled_orders") ==
              std::vector<std::string>{"2026-11-06T03:00:00+00:00"});
        CHECK(scalar(path, "SELECT COUNT(*) FROM feedback_reconciliation_audit") == 1);
        CHECK(column(path, "SELECT trading_day FROM feedback_reconciliation_audit") == std::vector<std::string>{"2026-11-05"});
    }
    json again = reconcile();
    for (const auto& plan : again["stores"]) {
        CHECK(plan["commands"].empty());
        CHECK(plan["orders"].empty());
    }
    // Applying needs a fresh backup directory.
    CHECK_THROWS_AS(reconcile(true, backup.string()), std::invalid_argument);
}

TEST_CASE_FIXTURE(PriorDayFixture, "a running consumer cannot restore a retained order but newer evidence can") {
    OrderFeedbackStore* live = feedback[0].get();
    live->record_snapshot(snapshot());
    CHECK(live->has_open_orders("A", "AEHL"));
    reconcile(true, (dir.path / "backup").string());
    CHECK_FALSE(live->has_open_orders("A", "AEHL"));
    // The same pre-existing connection replays the retained snapshot.
    live->record_snapshot(snapshot());
    live->record_snapshot(snapshot({{"sequence", 0}}));
    CHECK_FALSE(live->has_open_orders("A", "AEHL"));
    live->record_snapshot(snapshot({{"sequence", 2}, {"status", "NEW"}, {"updated_at", "2026-11-06T03:00:00Z"}}));
    CHECK(live->has_open_orders("A", "AEHL"));
    live->close();
    feedback[0] = std::make_unique<OrderFeedbackStore>(feedback_path(sources[0].state_db));
    CHECK(feedback[0]->has_open_orders("A", "AEHL"));
    feedback[0]->record_snapshot(snapshot({{"sequence", 1}}));
    CHECK(feedback[0]->has_open_orders("A", "AEHL"));
}

TEST_CASE_FIXTURE(PriorDayFixture, "pending command markers survive a repeated handoff") {
    json command = {{"account_id", "A"}, {"symbol", "POM"}, {"command_id", "limit_2026-10-26T15:00:00Z_1"}};
    feedback[0]->track_external_order(command);
    reconcile(true, (dir.path / "backup").string());
    feedback[0]->track_external_order(command);
    CHECK_FALSE(feedback[0]->has_open_orders("A", "POM"));
}

TEST_CASE_FIXTURE(PriorDayFixture, "an active prior-day child refuses cleanup without changing feedback") {
    feedback[0]->record_snapshot(snapshot());
    json parent = {{"order", {{"account_id", "A"}, {"broker_order_id", "old-order"}, {"client_order_id", "smart-1"},
                              {"state", "RECONCILE"}, {"send_wall", 1793130000}}}};
    {
        ng::Sqlite db(sources[0].state_db);
        ng::Statement insert(db, "INSERT INTO smart_parents VALUES ('session',1,?)");
        insert.bind(1, parent.dump());
        insert.exec();
    }
    try {
        reconcile(true, (dir.path / "backup").string());
        FAIL("the cleanup must be refused");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("active algorithm child") != std::string::npos);
    }
    CHECK(feedback[0]->has_open_orders("A", "AEHL"));
    CHECK(scalar(feedback_path(sources[0].state_db), "SELECT COUNT(*) FROM feedback_reconciliation_audit") == 0);
    // A terminal child no longer blocks the cleanup.
    {
        ng::Sqlite db(sources[0].state_db);
        parent["order"]["state"] = "TERMINAL";
        ng::Statement update(db, "UPDATE smart_parents SET data=?");
        update.bind(1, parent.dump());
        update.exec();
    }
    CHECK(reconcile()["stores"][0]["orders"].size() == 1);
}

TEST_CASE_FIXTURE(PriorDayFixture, "an old dispatched child without matching feedback still blocks the cleanup") {
    json parent = {{"order", {{"account_id", "Z"}, {"broker_order_id", nullptr}, {"client_order_id", "smart-2"},
                              {"state", "WORKING"}, {"send_wall", NOW - 3 * 86400}}}};
    ng::Sqlite db(sources[1].state_db);
    ng::Statement insert(db, "INSERT INTO smart_parents VALUES ('session',1,?)");
    insert.bind(1, parent.dump());
    insert.exec();
    CHECK_THROWS_WITH_AS(reconcile(), "A prior-day active algorithm child requires separate reconciliation", std::invalid_argument);
}

TEST_CASE_FIXTURE(PriorDayFixture, "a missing store or backup does not create a database or mutate") {
    CHECK_THROWS_WITH_AS(reconcile(true), "A backup directory is required when applying reconciliation", std::invalid_argument);
    std::string missing = dir.file("missing.sqlite3");
    std::vector<ReconcileSource> absent = {{"G", missing}};
    CHECK_THROWS_WITH_AS(reconcile(true, (dir.path / "backup").string(), &absent),
                         "Existing, separate state and feedback databases are required", std::invalid_argument);
    CHECK_FALSE(std::filesystem::exists(missing));
    CHECK_FALSE(std::filesystem::exists(dir.path / "backup"));
    std::vector<ReconcileSource> none;
    CHECK_THROWS_AS(reconcile(false, std::nullopt, &none), std::invalid_argument);
    std::vector<ReconcileSource> duplicated = {sources[0], sources[0]};
    CHECK_THROWS_AS(reconcile(false, std::nullopt, &duplicated), std::invalid_argument);
    CHECK_THROWS_WITH_AS(reconcile_prior_day(sources, "America/New_York", "   ", std::nullopt, false, NOW),
                         "An operator reason is required", std::invalid_argument);
    CHECK_THROWS_AS(reconcile_prior_day(sources, "Mars/Olympus", "reason", std::nullopt, false, NOW), std::invalid_argument);
}

namespace {

// A stopped session whose dispatched child was filled by the broker, reported without any client id.
struct StoppedChildFixture {
    TempDir dir{"reconcile-child"};
    std::string path = dir.file("state.sqlite3");
    std::string feedback_path = dir.file("feedback.sqlite3");
    double epoch = 1800000000;
    std::unique_ptr<SmartSellingEngine> engine;
    std::unique_ptr<SmartSellingFeedback> feedback;
    std::string parent_id, client_id;
    json child;
    json snapshot;

    StoppedChildFixture() {
        engine = std::make_unique<SmartSellingEngine>(path, [] { return 0.0; }, [this] { return epoch; });
        feedback = std::make_unique<SmartSellingFeedback>(*engine, 10000, feedback_path);
        parent_id = S(engine->start_parent(json{{"account_id", "A"}, {"symbol", "XYZ"}, {"quantity", 1000}, {"mode", "urgent"},
                                                {"horizon_seconds", 300}, {"tick_size", ".01"}, {"quantity_step", "1"},
                                                {"min_quantity", "100"}, {"min_notional", "0"}})["parent_id"]);
        QuoteUpdate quote;
        quote.bid = 10;
        quote.ask = 10.01;
        quote.bid_size = 1000;
        quote.source_time = epoch;
        engine->on_quote("XYZ", quote);
        TradeUpdate trade;
        trade.quantity = 1000;
        trade.event_id = "trade";
        trade.source_time = epoch;
        engine->on_trade("XYZ", trade);
        for (auto& action : engine->tick()) {
            if (action.at("type") == "submit") child = action;
        }
        REQUIRE(child.is_object());
        client_id = S(child["client_order_id"]);
        engine->prepare_dispatch(client_id);
        engine->request_stop(parent_id);
        std::string timestamp = ng::iso_utc_offset(epoch + 8);
        snapshot = {{"schema_version", 1}, {"account_id", "A"}, {"order_id", "broker-1"}, {"sequence", 1},
                    {"created_at", timestamp}, {"updated_at", timestamp}, {"symbol", "XYZ"}, {"side", "SELL"},
                    {"order_type", "LIMIT"}, {"status", "FILLED"}, {"quantity", 100}, {"filled_quantity", 100},
                    {"average_fill_price", 10}, {"limit_price", 10}};
        // A publisher that omits both client IDs reproduces the stuck lock.
        CHECK_FALSE(feedback->apply_snapshot(snapshot.dump()));
        CHECK(engine->get_parent(parent_id)["active"] == true);
    }
    json repair(bool apply = false) {
        return reconcile_stopped_child(path, feedback_path, client_id, snapshot, "order-status[0]@123", apply);
    }
};

}  // namespace

TEST_CASE_FIXTURE(StoppedChildFixture, "preview preserves live state and apply releases the account with the fill") {
    json before = engine->get_parent(parent_id);
    json preview = repair();
    CHECK(preview["applied"] == false);
    CHECK(preview["parent"]["active"] == false);
    CHECK(preview["source"] == "order-status[0]@123");
    CHECK(engine->get_parent(parent_id) == before);
    json result = repair(true);
    json after = engine->get_parent(parent_id);
    CHECK(result["applied"] == true);
    CHECK(after["active"] == false);
    CHECK(after["filled"] == "100");
    CHECK(after["reserved"] == "0");
    CHECK(after["active_order"].is_null());
    CHECK((*engine->get_order_context(client_id))["broker_order_id"] == "broker-1");
    json audit;
    for (const auto& event : engine->events(parent_id)) {
        if (event.at("kind") == "manual_terminal_reconciliation") audit = event;
    }
    REQUIRE(audit.is_object());
    CHECK(audit["data"]["snapshot"] == snapshot);
    CHECK(audit["data"]["source"] == "order-status[0]@123");
    CHECK(audit["data"]["previous_ledger"]["R"] == "100");
    CHECK(audit["data"]["client_order_id"] == client_id);
    CHECK(audit["data"]["previous_order"]["client_order_id"] == client_id);
    try {
        repair(true);
        FAIL("a resolved child cannot be reconciled twice");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("outstanding child") != std::string::npos);
    }
    CHECK(engine->get_parent(parent_id)["filled"] == "100");
}

TEST_CASE_FIXTURE(StoppedChildFixture, "repair preserves other sessions and the outbox") {
    json other = engine->start_parent(json{{"account_id", "B"}, {"symbol", "ABC"}, {"quantity", 1000}, {"tick_size", ".01"},
                                           {"quantity_step", "1"}, {"min_quantity", "100"}, {"min_notional", "0"}});
    auto outbox = [&] {
        ng::Sqlite db(path, ng::Sqlite::Mode::ReadOnly);
        ng::Statement rows(db, "SELECT id,data,dispatched FROM smart_outbox ORDER BY id");
        std::vector<json> out;
        while (rows.step()) out.push_back(rows.row_json());
        return out;
    };
    {
        ng::Sqlite db(path);
        db.exec("INSERT INTO smart_outbox(data,dispatched) VALUES ('{}',0)");
    }
    auto before = outbox();
    repair(true);
    CHECK(engine->get_parent(S(other["parent_id"])) == other);
    CHECK(outbox() == before);
}

TEST_CASE_FIXTURE(StoppedChildFixture, "nonterminal, conflicting and mismatched evidence is rejected") {
    json original = snapshot;
    for (const json& changes :
         {json{{"status", "UNKNOWN"}}, json{{"command_id", "another-command"}}, json{{"quantity", 101}, {"filled_quantity", 101}},
          json{{"limit_price", 11}}, json{{"account_id", "B"}}, json{{"symbol", "ABC"}}, json{{"sequence", 0}},
          json{{"created_at", ng::iso_utc_offset(epoch - 1)}}}) {
        CAPTURE(changes.dump());
        snapshot = original;
        snapshot.update(changes);
        CHECK_THROWS_AS(repair(true), std::invalid_argument);
        CHECK(engine->get_parent(parent_id)["reserved"] == "100");
    }
    snapshot = original;
    CHECK_THROWS_WITH_AS(reconcile_stopped_child(path, feedback_path, client_id, snapshot, "  ", true),
                         "A source reference for the reviewed evidence is required", std::invalid_argument);
    CHECK_THROWS_WITH_AS(reconcile_stopped_child(path, path, client_id, snapshot, "source", true),
                         "Existing, separate state and feedback databases are required", std::invalid_argument);
    CHECK_THROWS_WITH_AS(reconcile_stopped_child(path, feedback_path, "unknown-child", snapshot, "source", true),
                         "Unknown Smart selling child", std::invalid_argument);
}

TEST_CASE_FIXTURE(StoppedChildFixture, "newer unknown feedback blocks an old terminal snapshot") {
    json newer = snapshot;
    newer["sequence"] = 2;
    newer["status"] = "UNKNOWN";
    feedback->apply_snapshot(newer.dump());
    try {
        repair(true);
        FAIL("stale evidence must be rejected");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("latest durable") != std::string::npos);
    }
}

TEST_CASE_FIXTURE(StoppedChildFixture, "a broker order already owned by another child is rejected") {
    {
        ng::Sqlite db(path);
        ng::Statement select(db, "SELECT data FROM smart_orders WHERE id=?");
        select.bind(1, client_id);
        REQUIRE(select.step());
        json archived = json::parse(select.column_text(0));
        archived["client_order_id"] = "different-child";
        archived["broker_order_id"] = "broker-1";
        ng::Statement insert(db, "INSERT INTO smart_orders VALUES (?,?,?)");
        insert.bind(1, "different-child").bind(2, parent_id).bind(3, archived.dump());
        insert.exec();
    }
    try {
        repair(true);
        FAIL("the broker order is already bound");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("another child") != std::string::npos);
    }
    CHECK(engine->get_parent(parent_id)["reserved"] == "100");
}

TEST_CASE_FIXTURE(StoppedChildFixture, "a running session cannot be repaired") {
    {
        ng::Sqlite db(path);
        ng::Statement select(db, "SELECT data FROM smart_parents WHERE id=?");
        select.bind(1, parent_id);
        REQUIRE(select.step());
        json parent = json::parse(select.column_text(0));
        parent["stop_requested"] = false;
        ng::Statement update(db, "UPDATE smart_parents SET data=? WHERE id=?");
        update.bind(1, parent.dump()).bind(2, parent_id);
        update.exec();
    }
    try {
        repair(true);
        FAIL("running sessions are never repaired");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("stopped session") != std::string::npos);
    }
}

TEST_CASE("a stopped Smart buying child is resolved with its cash accounting") {
    TempDir dir("reconcile-buying");
    std::string path = dir.file("buying.sqlite3"), feedback_path = dir.file("buying.sqlite3.feedback.sqlite3");
    double epoch = 1800000000;
    ng::smart::SmartBuyingEngine engine(path, [] { return 0.0; }, [&] { return epoch; });
    SmartSellingFeedback feedback(engine, 10000, feedback_path);
    std::string parent_id = S(engine.start_parent(json{{"account_id", "A"}, {"symbol", "XYZ"}, {"quantity", "500"}, {"mode", "urgent"},
                                                      {"deadline", nullptr}, {"tick_size", ".01"}, {"quantity_step", "1"},
                                                      {"min_quantity", "100"}, {"min_notional", "0"},
                                                      {"account_budgets", {{"A", "10000"}}}})["parent_id"]);
    engine.set_cash_limit("A", Dec(10000));
    QuoteUpdate quote;
    quote.bid = "19.99";
    quote.ask = "20";
    quote.bid_size = 1000;
    quote.ask_size = 1000;
    quote.source_time = epoch;
    engine.on_quote("XYZ", quote);
    TradeUpdate trade;
    trade.quantity = 1000;
    trade.event_id = "trade";
    trade.source_time = epoch;
    engine.on_trade("XYZ", trade);
    json child;
    for (auto& action : engine.tick()) {
        if (action.at("type") == "submit") child = action;
    }
    REQUIRE(child.is_object());
    std::string client_id = S(child["client_order_id"]);
    engine.prepare_dispatch(client_id);
    engine.request_stop(parent_id);
    std::string timestamp = ng::iso_utc_offset(epoch + 8);
    json snapshot = {{"schema_version", 1}, {"account_id", "A"}, {"order_id", "broker-h"}, {"sequence", 1},
                     {"created_at", timestamp}, {"updated_at", timestamp}, {"symbol", "XYZ"}, {"side", "BUY"},
                     {"order_type", "LIMIT"}, {"status", "FILLED"}, {"quantity", 150}, {"filled_quantity", 150},
                     {"average_fill_price", 20}, {"limit_price", 20}};
    CHECK_FALSE(feedback.apply_snapshot(snapshot.dump()));
    json result = reconcile_stopped_child(path, feedback_path, client_id, snapshot, "order-status[0]@9", true);
    CHECK(result["applied"] == true);
    json after = engine.get_parent(parent_id);
    CHECK(after["active"] == false);
    CHECK(after["filled"] == "150");
    CHECK(after["reserved"] == "0");
    CHECK(after["accounts"]["A"]["spent"] == "3000");
    CHECK(after["trading_mode"] == "H");
}
