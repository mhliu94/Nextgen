// Algorithm sessions mirrored into the activity reports (tests/test_session_reporting.py).
#include "algo/smart_fixtures.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "algo/fast_trading/fast_trading.h"
#include "algo/smart_selling/runtime.h"
#include "orders/order_status.h"
#include "reporting/activity_reporting.h"
#include "reporting_fakes.h"

using namespace ngtest;
using ng::AccountDirectory;
using ng::AccountMeta;
using ng::AccountSnapshot;
using ng::BookLevel;
using ng::MarketDataStore;
using ng::OrderBookSnapshot;
using ng::Position;
using ng::fast::StrategyManager;
using ng::reporting::ActivityReporting;
using ng::smart::SmartSellingRuntime;

namespace {

json fast_command(std::initializer_list<std::pair<const char*, json>> updates = {}) {
    json command = {{"command_id", "fast-parent"}, {"trading_mode", "E"}, {"symbol", "AAPL"}, {"fast_trading_price_limit", 10},
                    {"fast_trading_account_ids", json::array({"A", "B"})}, {"fast_trading_book_levels", 2},
                    {"fast_trading_target_percentage", 100}, {"fast_trading_cycle_seconds", 60}};
    for (const auto& [key, value] : updates) command[key] = value;
    return command;
}

bool wait_for(const std::function<bool()>& done, int millis = 5000) {
    for (int i = 0; i < millis / 5 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return done();
}

struct FastHarness {
    AccountDirectory metas = directory({meta("A", 1, "Tiger"), meta("B", 2, "Tiger")});
    MarketDataStore market;
    std::mutex mutex;
    std::vector<json> published;
    std::function<void(const json&, const std::string&)> on_publish;
    double now = 0.0;
    bool simulated_clock = false;
    std::unique_ptr<StrategyManager> manager;

    explicit FastHarness(ng::reporting::SessionReporter* reporting, bool simulate_clock = false) : simulated_clock(simulate_clock) {
        OrderBookSnapshot book;
        book.symbol = "AAPL";
        book.bids.push_back(BookLevel{9.0, 1000.0, std::nullopt});
        book.asks.push_back(BookLevel{10.0, 1000.0, std::nullopt});
        market.upsert_book(book);
        manager = std::make_unique<StrategyManager>(
            market, [this]() -> const AccountDirectory& { return metas; },
            [this](const json& command, const std::string& account) {
                if (on_publish) on_publish(command, account);
                std::lock_guard lock(mutex);
                published.push_back(command);
            },
            nullptr, nullptr, ng::fast::RuntimeSettings::defaults(),
            simulated_clock ? StrategyManager::Clock([this] {
                std::lock_guard lock(mutex);
                return now;
            })
                            : StrategyManager::Clock(nullptr),
            reporting);
    }
    std::size_t count() {
        std::lock_guard lock(mutex);
        return published.size();
    }
    std::vector<json> commands() {
        std::lock_guard lock(mutex);
        return published;
    }
};

}  // namespace

TEST_CASE("fast reports start parameters, each child and stop") {
    RecordingReporter reporting;
    FastHarness h(&reporting);
    std::atomic<int> registered_before_publish{0};
    h.on_publish = [&](const json& command, const std::string&) {
        for (const auto& call : reporting.calls("register_command")) {
            if (call["session_id"] == "fast-parent" && call["command"] == command) ++registered_before_publish;
        }
    };
    json command = fast_command();
    h.manager->start_strategy(command);
    REQUIRE(wait_for([&] { return h.count() == 2; }));
    h.manager->stop_all();
    REQUIRE(wait_for([&] { return !reporting.calls("end_session").empty(); }));
    CHECK(registered_before_publish.load() == 2);

    auto starts = reporting.calls("start_session");
    REQUIRE(starts.size() == 1);
    CHECK(starts[0]["session_id"] == "fast-parent");
    CHECK(starts[0]["parameters"] == command);
    CHECK(starts[0]["account_ids"] == json::array({"A", "B"}));
    CHECK(starts[0]["started_at"].is_string());
    std::set<std::string> registered;
    for (const auto& call : reporting.calls("register_command")) registered.insert(call["command"]["account_id"].get<std::string>());
    CHECK(registered == std::set<std::string>{"A", "B"});
    bool stopping = false;
    for (const auto& call : reporting.calls("heartbeat_session")) {
        stopping = stopping || (call["session_id"] == "fast-parent" && call["status"] == "stopping");
    }
    CHECK(stopping);
    auto ended = reporting.calls("end_session");
    REQUIRE(ended.size() == 1);
    CHECK(ended[0] == json{{"method", "end_session"}, {"session_id", "fast-parent"}, {"status", "stopped"}, {"reason", "Manual stop"}});
}

TEST_CASE("fast test mode never registers simulated orders") {
    RecordingReporter reporting;
    FastHarness h(&reporting);
    ng::fast::CycleResult result = h.manager->run_cycle(fast_command({{"fast_trading_test_mode", true}}));
    CHECK_FALSE(result.commands.empty());
    CHECK(reporting.calls("register_command").empty());
    CHECK(h.count() == 0);
}

TEST_CASE("failed worker start does not leave a running report") {
    RecordingReporter reporting;
    FastHarness h(&reporting);
    h.manager->set_worker_spawner_for_test([](std::function<void()>) -> std::thread { throw std::runtime_error("thread unavailable"); });
    CHECK_THROWS_WITH_AS(h.manager->start_strategy(fast_command()), "thread unavailable", std::runtime_error);
    CHECK_FALSE(h.manager->has_active_session());
    CHECK_FALSE(h.manager->owns_account("A"));
    auto ended = reporting.calls("end_session");
    REQUIRE(ended.size() == 1);
    CHECK(ended[0] == json{{"method", "end_session"}, {"session_id", "fast-parent"}, {"status", "failed"},
                           {"reason", "Strategy worker failed to start"}});
}

TEST_CASE("fast long cycle heartbeats without changing the deadline") {
    RecordingReporter reporting;
    FastHarness h(&reporting, true);
    std::vector<double> waits;
    h.manager->set_cycle_waiter_for_test([&](double seconds) {
        std::lock_guard lock(h.mutex);
        waits.push_back(seconds);
        h.now += seconds;
        return waits.size() >= 6;  // stop when the first 60-second cycle wait has elapsed
    });
    h.manager->start_strategy(fast_command({{"fast_trading_test_mode", true}}));
    REQUIRE(wait_for([&] { return !h.manager->has_active_session(); }));
    REQUIRE(wait_for([&] { return !reporting.calls("end_session").empty(); }));
    {
        std::lock_guard lock(h.mutex);
        CHECK(h.now == 60);
        CHECK(waits == std::vector<double>(6, 10.0));
    }
    int running_heartbeats = 0;
    for (const auto& call : reporting.calls("heartbeat_session")) running_heartbeats += call["status"] == "running" ? 1 : 0;
    // One at the start of the cycle, then one before each ten-second slice of the wait.
    CHECK(running_heartbeats == 7);
}

TEST_CASE("fast ownership persistence failure prevents order publication") {
    RecordingReporter reporting;
    reporting.fail("register_command");
    FastHarness h(&reporting);
    CHECK_THROWS(h.manager->run_cycle(fast_command()));
    CHECK(h.count() == 0);
}

namespace {

struct SmartHarness {
    TempDir dir{"session-reporting"};
    double now = 1800000000.0;
    AccountDirectory metas = directory({meta("A", 1, "Tiger"), meta("B", 2, "Tiger")});
    std::map<std::string, AccountSnapshot> snapshots;
    std::vector<json> published;
    std::function<void(const json&)> on_publish;
    std::shared_ptr<SmartSellingEngine> engine;
    std::unique_ptr<SmartSellingRuntime> runtime;

    explicit SmartHarness(ng::reporting::SessionReporter* reporting) {
        for (const char* account : {"A", "B"}) {
            AccountSnapshot s;
            s.account_id = account;
            s.cash = 0;
            s.positions.push_back(Position{"XYZ", 1000, std::nullopt, 1000});
            s.ts = ng::iso_utc_offset(now);
            s.trading_enabled = true;
            snapshots[account] = s;
        }
        engine = make_engine();
        runtime = make_runtime(engine, reporting);
    }
    ~SmartHarness() { runtime->close(); }

    std::shared_ptr<SmartSellingEngine> make_engine() {
        return std::make_shared<SmartSellingEngine>(dir.file("smart.sqlite3"), [] { return 0.0; }, [this] { return now; });
    }
    std::unique_ptr<SmartSellingRuntime> make_runtime(std::shared_ptr<SmartSellingEngine> use, ng::reporting::SessionReporter* reporting) {
        auto built = std::make_unique<SmartSellingRuntime>(
            ng::normalize_smart_selling_config(nullptr), [this]() -> const AccountDirectory& { return metas; },
            [this] { return snapshots; },
            [this](const json& command, const std::string&) {
                if (on_publish) on_publish(command);
                published.push_back(command);
            },
            nullptr, std::move(use), [] { return 0.0; }, [this] { return now; }, reporting);
        built->set_feed_states_for_test({{"orders", "live"}, {"volume", "live"}});
        return built;
    }
    json start() {
        return runtime->start_parent(json{{"account_ids", json::array({"A", "B"})},
                                          {"symbol", "XYZ"},
                                          {"mode", "urgent"},
                                          {"deadline", ng::iso_utc_offset(now + 300)},
                                          {"minimum_price", "9.5"}});
    }
    // Quote 10 / 10.01 x 1000 and 2000 shares of fresh tape, then one heartbeat.
    void trade_and_tick() {
        QuoteUpdate quote;
        quote.bid = 10;
        quote.ask = 10.01;
        quote.bid_size = 1000;
        quote.source_time = now;
        engine->on_quote("XYZ", quote);
        TradeUpdate trade;
        trade.quantity = 2000;
        trade.event_id = "t1";
        trade.source_time = now;
        engine->on_trade("XYZ", trade);
        runtime->tick();
        runtime->wait_idle();
    }
};

bool has_heartbeat(const RecordingReporter& reporting, const json& session_id, const char* status) {
    for (const auto& call : reporting.calls("heartbeat_session")) {
        if (call["session_id"] == session_id && call["status"] == status) return true;
    }
    return false;
}

ng::orders::OrderEvent filled_event(const json& command, const std::string& order_id, json filled = 2, json price = 9.75,
                                    std::initializer_list<std::pair<const char*, json>> changes = {}) {
    json payload = {{"schema_version", 1}, {"account_id", command["account_id"]}, {"order_id", order_id}, {"sequence", 1},
                    {"created_at", command["ts"]}, {"updated_at", command["ts"]}, {"symbol", command["symbol"]},
                    {"side", command["side"]}, {"order_type", "LIMIT"}, {"quantity", command["qty_shares"]},
                    {"filled_quantity", filled}, {"average_fill_price", price}, {"limit_price", command["limit_price"]},
                    {"status", "PARTIALLY_FILLED"}, {"currency", "USD"}, {"command_id", command["command_id"]}};
    for (const auto& [key, value] : changes) payload[key] = value;
    return ng::orders::parse_order_message(payload.dump());
}

}  // namespace

TEST_CASE("smart reports effective parameters, all accounts and terminal stop") {
    RecordingReporter reporting;
    SmartHarness h(&reporting);
    json parent = h.start();
    auto starts = reporting.calls("start_session");
    REQUIRE(starts.size() == 1);
    CHECK(starts[0]["session_id"] == parent["parent_id"]);
    CHECK(starts[0]["account_ids"] == json::array({"A", "B"}));
    const json& parameters = starts[0]["parameters"];
    CHECK(parameters["quantity"] == "2000");
    CHECK(parameters["minimum_price"] == "9.5");
    CHECK(ng::json_python_truthy(&parameters["participation_percentage"]));
    CHECK(ng::json_python_truthy(&parameters["instrument_rules"]["quantity_step"]));
    h.runtime->on_feed_state("volume", "disconnected");
    CHECK(has_heartbeat(reporting, parent["parent_id"], "paused"));
    h.runtime->stop_parent(S(parent["parent_id"]));
    auto ended = reporting.calls("end_session");
    REQUIRE_FALSE(ended.empty());
    CHECK(ended.back()["session_id"] == parent["parent_id"]);
    CHECK(ended.back()["status"] == "stopped");
    CHECK(reporting.calls("start_session").size() == 1);
}

TEST_CASE("smart registers a confirmable child before publication") {
    RecordingReporter reporting;
    SmartHarness h(&reporting);
    json parent = h.start();
    int registered_first = 0;
    h.on_publish = [&](const json& command) {
        for (const auto& call : reporting.calls("register_command")) {
            if (call["session_id"] == parent["parent_id"] && call["command"] == command) ++registered_first;
        }
    };
    h.trade_and_tick();
    REQUIRE(h.published.size() == 1);
    CHECK(registered_first == 1);
    const json& child = h.published[0];
    CHECK(child["command_id"] == child["client_order_id"]);
    CHECK(child["parent_id"] == parent["parent_id"]);
    h.runtime->stop_parent(S(parent["parent_id"]));
    CHECK(has_heartbeat(reporting, parent["parent_id"], "stopping"));
    CHECK(reporting.calls("end_session").empty());
}

TEST_CASE("smart ownership persistence failure blocks new orders but heartbeat failure does not") {
    for (const char* failing : {"start_session", "register_command", "heartbeat_session"}) {
        CAPTURE(std::string(failing));
        RecordingReporter reporting;
        reporting.fail(failing);
        SmartHarness h(&reporting);
        json parent = h.start();
        CHECK(h.runtime->has_active_session());
        h.trade_and_tick();
        if (std::string(failing) != "heartbeat_session") {
            CHECK(h.published.empty());
            continue;
        }
        REQUIRE(h.published.size() == 1);
        CHECK(h.published[0]["parent_id"] == parent["parent_id"]);
    }
}

TEST_CASE("smart recovers the active session and child correlation") {
    SmartHarness h(nullptr);
    json parent = h.start();
    h.trade_and_tick();
    REQUIRE(h.published.size() == 1);
    json child = h.published[0];
    RecordingReporter reporting;
    auto recovered = h.make_runtime(h.make_engine(), &reporting);
    auto starts = reporting.calls("start_session");
    REQUIRE_FALSE(starts.empty());
    CHECK(starts.back()["session_id"] == parent["parent_id"]);
    CHECK(starts.back()["account_ids"] == json::array({"A", "B"}));
    CHECK(has_heartbeat(reporting, parent["parent_id"], "reconciling"));
    auto registered = reporting.calls("register_command");
    REQUIRE_FALSE(registered.empty());
    CHECK(registered.back()["command"]["command_id"] == child["command_id"]);
    CHECK(reporting.calls("end_session").empty());
    recovered->close();
}

TEST_CASE("fast real reporting persists parameters and only correlated fills") {
    TempDir dir("fast-real-reporting");
    std::string path = dir.file("activity.sqlite3");
    auto writer = std::make_unique<ActivityReporting>(path);
    auto reader = std::make_unique<ActivityReporting>(path);
    FastHarness h(writer.get());
    json command = fast_command();
    h.manager->start_strategy(command);
    REQUIRE(wait_for([&] { return h.count() == 2; }));
    json session = reader->snapshot(h.metas)["sessions"][0];
    CHECK(session["parameters"] == command);
    CHECK(session["account_ids"] == json::array({"A", "B"}));
    CHECK(session["running"] == true);
    CHECK(session["totals"]["shares_bought"] == 0.0);
    std::vector<json> commands = h.commands();
    for (std::size_t index = 0; index < commands.size(); ++index) {
        reader->record_order(filled_event(commands[index], "broker-" + std::to_string(index)));
    }
    json unrelated = commands[0];
    unrelated["command_id"] = "unrelated-manual-command";
    reader->record_order(filled_event(unrelated, "manual", 100));
    h.manager->stop_all();
    REQUIRE(wait_for([&] { return reader->snapshot(h.metas)["sessions"][0]["status"] == "stopped"; }));
    session = reader->snapshot(h.metas)["sessions"][0];
    CHECK(session["status"] == "stopped");
    CHECK(session["running"] == false);
    CHECK(session["reason"] == "Manual stop");
    CHECK(session["totals"]["shares_bought"] == 4.0);
    CHECK(session["totals"]["cash_by_currency"]["USD"]["cash_change"] == -39.0);
    REQUIRE(session["accounts"].size() == 2);
    for (const auto& row : session["accounts"]) CHECK(row["shares_bought"] == 2.0);
    h.manager.reset();
    writer->close();
    reader->close();
    ActivityReporting reopened(path);
    session = reopened.snapshot(h.metas)["sessions"][0];
    CHECK(session["parameters"] == command);
    CHECK(session["ended_at"].is_string());
    CHECK(session["totals"]["shares_bought"] == 4.0);
}

TEST_CASE("smart real reporting serializes effective settings and tracks fill then stop") {
    TempDir dir("smart-real-reporting");
    std::string path = dir.file("activity.sqlite3");
    double* now_pointer = nullptr;
    auto clock = [&] { return *now_pointer; };
    double initial_now = 1800000000.0;
    now_pointer = &initial_now;
    ActivityReporting writer(path, clock), reader(path, clock);
    SmartHarness h(&writer);
    now_pointer = &h.now;
    json parent = h.start();
    json initial = reader.snapshot(h.metas)["sessions"][0];
    CHECK(initial["id"] == parent["parent_id"]);
    CHECK(initial["parameters"]["quantity"] == "2000");
    CHECK(initial["parameters"]["minimum_price"] == "9.5");
    CHECK(ng::json_python_truthy(&initial["parameters"]["instrument_rules"]["quantity_step"]));
    CHECK(initial["parameters"]["account_quantities"] == json{{"A", "1000"}, {"B", "1000"}});
    CHECK(initial["account_ids"] == json::array({"A", "B"}));
    CHECK(initial["running"] == true);
    h.trade_and_tick();
    REQUIRE(h.published.size() == 1);
    json child = h.published[0];
    ng::orders::OrderEvent event = filled_event(child, "smart-broker", 3, 10);
    reader.record_order(event);
    h.runtime->on_order_payload(event.payload.dump());
    h.runtime->stop_parent(S(parent["parent_id"]));
    h.runtime->wait_idle();
    json stopping = reader.snapshot(h.metas)["sessions"][0];
    CHECK(stopping["status"] == "stopping");
    ng::orders::OrderEvent final_event = filled_event(child, "smart-broker", 3, 10, {{"status", "CANCELED"}, {"sequence", 2}});
    reader.record_order(final_event);
    h.runtime->on_order_payload(final_event.payload.dump());
    json stopped = reader.snapshot(h.metas)["sessions"][0];
    CHECK(stopped["status"] == "stopped");
    CHECK(stopped["running"] == false);
    CHECK(stopped["parameters"] == initial["parameters"]);
    CHECK(stopped["started_at"] == initial["started_at"]);
    CHECK(stopped["totals"]["shares_sold"] == 3.0);
    CHECK(stopped["totals"]["cash_by_currency"]["USD"]["cash_change"] == 30.0);
    CHECK(stopped["accounts"].size() == 2);
}
