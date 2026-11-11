// Shared session admission across modes E/F/G/H and the manual-order guards
// (tests/test_algo_sessions.py), exercised through the application's ControlCenter.
#include "web_fixture.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <set>

using ng::json;
using namespace ng::web;
using ngtest::body;
using ngtest::WebFixture;

namespace {

const char* kConflict = "must use distinct symbols";

struct SessionFixture : WebFixture {
    json smart_settings = {{"account_ids", json::array({"B"})}, {"symbol", "XYZ"}, {"deadline", nullptr}};

    SessionFixture()
        : WebFixture(R"({"accounts": [
            {"string_id": "A", "numeric_id": 1, "broker": "Tiger", "trading_medium": "API"},
            {"string_id": "B", "numeric_id": 2, "broker": "Tiger", "trading_medium": "API"},
            {"string_id": "C", "numeric_id": 3, "broker": "Tiger", "trading_medium": "API"}]})") {
        for (const char* account : {"A", "B", "C"}) {
            ng::AccountSnapshot snapshot;
            snapshot.account_id = account;
            snapshot.cash = 10000;
            snapshot.ts = ng::iso_utc_now();
            snapshot.trading_enabled = true;
            for (const char* symbol : {"XYZ", "OTHER", "AAPL", "MSFT"})
                snapshot.positions.push_back(ng::Position{symbol, 1000, 9.0, 1000});
            control->account_store().upsert(snapshot);
        }
        for (const char* symbol : {"XYZ", "OTHER", "AAPL", "MSFT"}) upsert_book(symbol, 10, 10.01);
    }
    json fast(const std::string& command_id, const std::string& mode, const std::string& symbol, std::vector<std::string> accounts) {
        return fast_start(command_id, mode, symbol, std::move(accounts), true);
    }
    std::atomic<int> sequence{0};
    json start_mode(const std::string& mode, const std::string& symbol, const std::vector<std::string>& accounts) {
        if (mode == "E" || mode == "F") {
            auto command = fast("matrix-" + std::to_string(++sequence), mode, symbol, accounts);
            control->fast_manager()->start_strategy(command); // exercise direct admission, outside ControlCenter's lock
            return command;
        }
        auto* runtime = mode == "G" ? control->smart_runtime() : control->buying_runtime();
        return runtime->start_parent(json{{"account_ids", accounts}, {"symbol", symbol}, {"deadline", nullptr}});
    }
    void stop_modes() {
        control->fast_manager()->stop_all();
        control->smart_runtime()->stop_all();
        control->buying_runtime()->stop_all();
        REQUIRE(wait_until([&] { return !control->any_algorithm_active(); }));
    }
    json fast_command() { return fast("fast-session", "E", "AAPL", {"A"}); }
    bool conflicts(const std::function<void()>& start) {
        try {
            start();
        } catch (const std::invalid_argument& e) {
            return std::string(e.what()).find(kConflict) != std::string::npos;
        }
        return false;
    }
};

// Blocks the STOP_ALGO_TRADING notification until released; records everything else.
class GatedSink : public CommandSink {
public:
    void publish(const json& command, const std::string& key) override {
        std::unique_lock lock(mutex_);
        if (command.value("type", "") == "STOP_ALGO_TRADING") {
            publishing_stop_ = true;
            cv_.notify_all();
            cv_.wait_for(lock, std::chrono::seconds(10), [this] { return released_; });
        }
        history_.push_back(ng::kafka::PublishedCommand{ng::wall_now(), key, command, "recorded", ""});
    }
    std::vector<ng::kafka::PublishedCommand> history() const override {
        std::lock_guard lock(mutex_);
        return history_;
    }
    bool wait_for_stop_notification() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return publishing_stop_; });
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool publishing_stop_ = false;
    bool released_ = false;
    std::vector<ng::kafka::PublishedCommand> history_;
};

}  // namespace

TEST_CASE_FIXTURE(SessionFixture, "sessions in every mode run at once when their accounts are disjoint") {
    control->start_fast_strategy(fast("buy", "E", "AAPL", {"A"}));
    control->start_fast_strategy(fast("sell", "F", "MSFT", {"C"}));
    json parent = control->start_smart_parent(smart_settings);
    CHECK(control->fast_manager()->statuses().size() == 2);
    CHECK(control->smart_runtime()->has_active_session());
    CHECK(control->fast_manager()->owns_account("A"));
    CHECK(control->fast_manager()->owns_account("C"));
    CHECK(control->smart_runtime()->owns_account("B"));
    CHECK_FALSE(control->smart_runtime()->owns_account("A"));
    CHECK_FALSE(control->fast_manager()->owns_account("B"));
    control->stop_smart_parent(parent["parent_id"].get<std::string>());
    CHECK(control->fast_manager()->stop_all() == 2);
}

TEST_CASE_FIXTURE(SessionFixture, "one account runs one buyer and multiple sellers independently") {
    start_mode("E", "AAPL", {"A"});
    start_mode("F", "MSFT", {"A"});
    auto parent = start_mode("G", "XYZ", {"A"});
    start_mode("G", "OTHER", {"A"});
    CHECK(control->fast_manager()->statuses().size() == 2);
    CHECK(control->smart_runtime()->algo_sessions().size() == 2);
    CHECK(conflicts([&] { start_mode("F", "MSFT", {"C", "A"}); }));
    CHECK(conflicts([&] { start_mode("H", "XYZ", {"C", "A"}); }));
    CHECK_FALSE(control->fast_manager()->owns_account("C"));
    CHECK_FALSE(control->buying_runtime()->owns_account("C"));
    control->stop_smart_parent(parent["parent_id"].get<std::string>());
    CHECK_NOTHROW(start_mode("G", "XYZ", {"A"}));
    auto stopped = control->stop_all_algorithms("test");
    CHECK(stopped.fast_stopped == 2);
    CHECK(stopped.smart_stopped == 2);
    CHECK(wait_until([&] { return !control->any_algorithm_active(); }));
}

TEST_CASE_FIXTURE(SessionFixture, "every algorithm mode pair rejects the same canonical symbol") {
    for (const auto& first : {"E", "F", "G", "H"}) {
        for (const auto& second : {"E", "F", "G", "H"}) {
            CAPTURE(first); CAPTURE(second);
            start_mode(first, "XYZ", {"A"});
            try {
                start_mode(second, " us.xyz/USD ", {"B", "A"});
                FAIL("Overlapping symbol admitted");
            } catch (const std::invalid_argument& error) {
                std::string message = error.what();
                CHECK(message.find("Account(s) A:") != std::string::npos);
                CHECK(message.find(kConflict) != std::string::npos);
                CHECK(message.find(std::string("mode ") + first + ", symbol XYZ") != std::string::npos);
            }
            CHECK_FALSE(control->fast_manager()->owns_account("B"));
            CHECK_FALSE(control->smart_runtime()->owns_account("B"));
            CHECK_FALSE(control->buying_runtime()->owns_account("B"));
            stop_modes();
        }
    }
}

TEST_CASE_FIXTURE(SessionFixture, "distinct symbols permit every mode pair except two buyers") {
    for (const std::string first : {"E", "F", "G", "H"}) {
        for (const std::string second : {"E", "F", "G", "H"}) {
            CAPTURE(first); CAPTURE(second);
            start_mode(first, "XYZ", {"A"});
            if ((first == "E" || first == "H") && (second == "E" || second == "H")) {
                CHECK_THROWS_WITH(start_mode(second, "OTHER", {"B", "A"}), doctest::Contains("at most 1 buying strategy"));
                CHECK_FALSE(control->fast_manager()->owns_account("B"));
                CHECK_FALSE(control->buying_runtime()->owns_account("B"));
            } else {
                CHECK_NOTHROW(start_mode(second, "OTHER", {"B", "A"}));
            }
            CHECK_NOTHROW(start_mode(second, "XYZ", {"C"}));
            stop_modes();
        }
    }
}

TEST_CASE_FIXTURE(SessionFixture, "simultaneous direct runtime starts obey symbol and buying reservations") {
    struct Pair { std::string first, second, symbol; int admitted; };
    for (const Pair& pair : {Pair{"E", "H", "OTHER", 1}, {"H", "H", "OTHER", 1}, {"F", "G", "XYZ", 1},
                           {"G", "G", "XYZ", 1}, {"E", "G", "OTHER", 2}, {"H", "F", "OTHER", 2},
                           {"F", "G", "OTHER", 2}, {"G", "G", "OTHER", 2}}) {
        CAPTURE(pair.first); CAPTURE(pair.second); CAPTURE(pair.symbol);
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        auto attempt = [&](const std::string& mode, const std::string& symbol) {
            ++ready;
            while (!go.load()) std::this_thread::yield();
            try { start_mode(mode, symbol, {"A"}); return 1; }
            catch (const std::invalid_argument&) { return 0; }
        };
        auto first = std::async(std::launch::async, attempt, pair.first, "XYZ");
        auto second = std::async(std::launch::async, attempt, pair.second, pair.symbol);
        REQUIRE(wait_until([&] { return ready == 2; }));
        go = true;
        CHECK(first.get() + second.get() == pair.admitted);
        stop_modes();
    }
}

TEST_CASE_FIXTURE(SessionFixture, "scheduled paused buyers keep cash reserved and failed starts reserve nothing") {
    auto parent = control->buying_runtime()->start_parent(json{{"account_ids", {"A"}}, {"symbol", "XYZ"},
        {"start_time", ng::wall_now() + 3600}, {"deadline", nullptr}});
    control->buying_runtime()->on_feed_state("orders", "disconnected");
    CHECK_THROWS_WITH(start_mode("E", "OTHER", {"A"}), doctest::Contains("at most 1 buying strategy"));
    CHECK_NOTHROW(start_mode("F", "OTHER", {"A"}));
    control->stop_smart_buying_parent(parent["parent_id"].get<std::string>());
    control->buying_runtime()->on_feed_state("orders", "live");
    CHECK_THROWS_WITH(control->buying_runtime()->start_parent(json{{"account_ids", {"A"}}, {"symbol", "XYZ"}, {"mode", "invalid"}}),
                      doctest::Contains("mode must"));
    CHECK_NOTHROW(start_mode("E", "XYZ", {"A"}));
}

TEST_CASE_FIXTURE(SessionFixture, "all start APIs identify the account mode symbol and conflicting rule") {
    start_mode("E", "AAPL", {"A"});
    auto c = client();
    for (const std::string mode : {"E", "F", "G", "H"}) {
        CAPTURE(mode);
        bool buying = mode == "E" || mode == "H";
        std::string symbol = buying ? "MSFT" : "AAPL";
        json payload = mode == "E" || mode == "F" ? fast("api", mode, symbol, {"B", "A"})
            : json{{"account_ids", {"B", "A"}}, {"symbol", symbol}, {"deadline", nullptr}};
        std::string path = mode == "E" || mode == "F" ? "/api/submit-algo" : mode == "G" ? "/api/smart-selling" : "/api/smart-buying";
        auto response = c.Post(path, payload.dump(), "application/json");
        REQUIRE(response);
        CHECK(response->status == 400);
        auto result = body(response);
        CHECK(result["ok"] == false);
        std::string error = result["error"].get<std::string>();
        CHECK(error.find("Account(s) A:") != std::string::npos);
        CHECK(error.find("mode E, symbol AAPL") != std::string::npos);
        CHECK(error.find(buying ? "at most 1 buying strategy" : kConflict) != std::string::npos);
    }
}

TEST_CASE_FIXTURE(SessionFixture, "concurrent starts on one account admit exactly one session") {
    for (int round = 0; round < 5; ++round) {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        auto attempt = [&](const std::function<void()>& start) {
            ++ready;
            while (!go.load()) std::this_thread::yield();
            try {
                start();
                return true;
            } catch (const std::invalid_argument&) {
                return false;
            }
        };
        json contested = smart_settings;
        contested["account_ids"] = json::array({"A"});
        contested["symbol"] = "AAPL";
        std::string id = "fast-" + std::to_string(round);
        auto first = std::async(std::launch::async, attempt, [&] { control->start_fast_strategy(fast(id, "E", "AAPL", {"A"})); });
        auto second = std::async(std::launch::async, attempt, [&] { control->start_smart_parent(contested); });
        while (ready.load() < 2) std::this_thread::yield();
        go = true;
        bool fast_won = first.get();
        bool smart_won = second.get();
        CHECK(fast_won != smart_won);
        CHECK(control->fast_manager()->owns_account("A") != control->smart_runtime()->owns_account("A"));
        control->fast_manager()->stop_all();
        control->smart_runtime()->stop_all();
        REQUIRE(wait_until([&] { return !control->any_algorithm_active(); }));
    }
}

TEST_CASE_FIXTURE(SessionFixture, "manual orders and cancellations are blocked for entire owned accounts") {
    control->start_fast_strategy(fast_command());
    control->start_smart_parent(smart_settings);
    for (const char* account : {"A", "B"}) {
        for (const char* kind : {"MARKET_ORDER", "DELAYED_MARKET_ORDER", "LIMIT_ORDER", "LIMIT_ORDER_FOK", "CANCEL_OPEN_ORDERS",
                                 "CURRENCY_CONVERSION"}) {
            for (const json& symbol : {json("XYZ"), json("OTHER"), json(nullptr)}) {
                CAPTURE(std::string(account));
                CAPTURE(std::string(kind));
                json command = {{"type", kind}, {"account_id", account}, {"symbol", symbol}};
                try {
                    control->publish_control_command(command, account);
                    FAIL("the guard must reject the command");
                } catch (const std::invalid_argument& e) {
                    CHECK(std::string(e.what()).find("owns this account") != std::string::npos);
                }
            }
        }
    }
    CHECK(sink->count() == 0);
    // An unowned account is unaffected, and read-only requests are never blocked.
    control->publish_control_command(
        json{{"type", "MARKET_ORDER"}, {"command_id", "manual-c"}, {"account_id", "C"}, {"symbol", "XYZ"}, {"qty_shares", 1}}, "C");
    control->publish_control_command(json{{"type", "GET_BROKER_ORDERS"}, {"account_ids", json::array({"A", "B"})}}, "GET_BROKER_ORDERS");
    control->publish_control_command(json{{"type", "SET_TRADING_ENABLED"}, {"account_id", "A"}, {"trading_enabled", false}}, "A");
    CHECK(sink->count() == 3);
}

TEST_CASE_FIXTURE(SessionFixture, "manual cancellations on unselected accounts remain available") {
    control->start_smart_parent(smart_settings);
    auto c = client();
    auto allowed = c.Post("/submit-cancel-open-orders", "account_ids=A", "application/x-www-form-urlencoded");
    REQUIRE(allowed);
    CHECK(allowed->get_header_value("Location").find("ok=") != std::string::npos);
    REQUIRE(sink->count() == 1);
    CHECK(sink->history().back().command["account_id"] == "A");
    sink->clear();
    auto blocked = c.Post("/submit-cancel-open-orders", "account_ids=B", "application/x-www-form-urlencoded");
    REQUIRE(blocked);
    CHECK(blocked->get_header_value("Location").find("err=") != std::string::npos);
    CHECK(sink->count() == 0);
}

TEST_CASE_FIXTURE(SessionFixture, "stop all stops smart even if the notification transport fails") {
    control->start_smart_parent(smart_settings);
    sink->fail_next("transport unavailable");
    auto response = client().Post("/submit-algo-stop", "", "application/x-www-form-urlencoded");
    REQUIRE(response);
    CHECK(response->status == 303);
    CHECK_FALSE(control->smart_runtime()->has_active_session());
    std::string location = response->get_header_value("Location");
    CHECK(location.find("err=") != std::string::npos);
    CHECK(location.find("ok=") != std::string::npos);
}

TEST_CASE_FIXTURE(SessionFixture, "stop all ends every running session") {
    control->start_fast_strategy(fast("buy", "E", "AAPL", {"A"}));
    control->start_fast_strategy(fast("sell", "F", "MSFT", {"C"}));
    control->start_smart_parent(smart_settings);
    auto response = client().Post("/submit-algo-stop", "", "application/x-www-form-urlencoded");
    REQUIRE(response);
    CHECK(response->status == 303);
    CHECK(response->get_header_value("Location").find("ok=") != std::string::npos);
    CHECK(wait_until([&] { return !control->fast_manager()->has_active_session(); }));
    CHECK_FALSE(control->smart_runtime()->has_active_session());
    CHECK(sink->history().back().command["type"] == "STOP_ALGO_TRADING");
    CHECK(sink->history().back().command["scope"] == "ALL");
}

TEST_CASE_FIXTURE(SessionFixture, "a new start waits until the previous global stop notification is enqueued") {
    auto gated = std::make_unique<GatedSink>();
    GatedSink* gate = gated.get();
    control->set_command_sink(std::move(gated));
    sink = nullptr;
    control->start_smart_parent(smart_settings);
    auto c = client();
    auto stop = std::async(std::launch::async, [&] {
        auto response = c.Post("/submit-algo-stop", "", "application/x-www-form-urlencoded");
        return response ? response->status : -1;
    });
    REQUIRE(gate->wait_for_stop_notification());
    std::atomic<bool> starting{false};
    auto start = std::async(std::launch::async, [&] {
        starting = true;
        control->start_fast_strategy(fast_command());
        // The stop notification was enqueued before this session could start.
        auto history = gate->history();
        return history.size() == 1 && history[0].command["type"] == "STOP_ALGO_TRADING";
    });
    REQUIRE(wait_until([&] { return starting.load(); }));
    CHECK(start.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    gate->release();
    CHECK(stop.get() == 303);
    REQUIRE(start.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(start.get());
    CHECK(control->fast_manager()->owns_account("A"));
}

TEST_CASE_FIXTURE(SessionFixture, "algorithm order handoff bypasses the manual guard but not a stop") {
    control->start_fast_strategy(fast_command());
    json command = {{"type", "LIMIT_ORDER_FOK"}, {"account_id", "A"}, {"symbol", "AAPL"}, {"command_id", "child-1"}};
    control->publish_fast_trading_command(command, "A");
    REQUIRE(sink->count() == 1);
    CHECK(sink->history()[0].command == command);
    CHECK(sink->history()[0].key == "A");
    control->fast_manager()->stop_all();
    sink->clear();
    control->publish_fast_trading_command(command, "A");
    CHECK(sink->count() == 0);
}

TEST_CASE_FIXTURE(SessionFixture, "fast handoff proceeds while Smart shares the account on another symbol") {
    control->start_fast_strategy(fast_command());
    start_mode("G", "XYZ", {"A"});
    json command = {{"type", "LIMIT_ORDER_FOK"}, {"account_id", "A"}, {"symbol", "AAPL"}, {"command_id", "child-1"}};
    control->publish_fast_trading_command(command, "A");
    REQUIRE(sink->count() == 1);
    CHECK(sink->history()[0].key == "A");
    // A conflicting claim on the same account AND symbol still halts the handoff.
    // Drive the engine directly to model such an inconsistent claim.
    sink->clear();
    control->smart_runtime()->engine().start_parent(
        json{{"account_id", "A"}, {"symbol", "AAPL"}, {"quantity", "1000"}, {"mode", "urgent"}, {"deadline", nullptr},
             {"tick_size", ".01"}, {"quantity_step", "1"}, {"min_quantity", "100"}, {"min_notional", "0"}});
    REQUIRE(control->smart_runtime()->owns_account("A"));
    CHECK(conflicts([&] { control->publish_fast_trading_command(command, "A"); }));
    CHECK(sink->count() == 0);
}

// tests/test_admin_control.py: AdminControlEndpointTests (JSON API of the single-page UI and the form route).
TEST_CASE_FIXTURE(SessionFixture, "broker order history requests are limited to lmh and published once") {
    std::string date = "2024-02-29";
    std::string body_json = json{{"trading_date", date}, {"account_ids", json::array({"A"})}}.dump();
    // Anonymous: the API answers 401, the form route redirects to sign-in; nothing is published.
    CHECK(client("").Post("/api/broker-orders", body_json, "application/json")->status == 401);
    auto anonymous_form = client("").Post("/submit-broker-orders", "trading_date=" + date + "&account_ids=A", "application/x-www-form-urlencoded");
    CHECK(anonymous_form->status == 303);
    CHECK(ng::starts_with(anonymous_form->get_header_value("Location"), "/login?next="));
    for (const char* user : {"admin", "trader", "LMH", " lmh "}) {
        CAPTURE(std::string(user));
        auto c = client_for(user);
        auto api = c.Post("/api/broker-orders", body_json, "application/json");
        CHECK(api->status == 403);
        CHECK(body(api)["error"] == ng::i18n::t("en", "admin_control_forbidden"));
        CHECK(c.Post("/submit-broker-orders", "trading_date=" + date + "&account_ids=A", "application/x-www-form-urlencoded")->status == 403);
    }
    CHECK(sink->count() == 0);

    // One multi-account request, without touching the algorithms that own those accounts.
    control->start_fast_strategy(fast_command());
    control->start_smart_parent(smart_settings);
    auto lmh = client_for("lmh");
    auto response = lmh.Post("/api/broker-orders", json{{"trading_date", date}, {"account_ids", json::array({"B", "A", "B"})}}.dump(),
                             "application/json");
    REQUIRE(response);
    CHECK(response->status == 200);
    json result = body(response);
    CHECK(result["ok"] == true);
    REQUIRE(sink->count() == 1);
    auto published = sink->history()[0];
    CHECK(published.key == "GET_BROKER_ORDERS");
    CHECK(published.command == result["command"]);
    CHECK(published.command["type"] == "GET_BROKER_ORDERS");
    CHECK(published.command["account_ids"] == json::array({"A", "B"}));
    CHECK(published.command["trading_date"] == date);
    CHECK(published.command["timezone"] == "America/New_York");
    CHECK_FALSE(published.command.contains("account_id"));
    CHECK(result["message"].get<std::string>().find("2 account(s)") != std::string::npos);
    CHECK(result["message"].get<std::string>().find(published.command["command_id"].get<std::string>()) != std::string::npos);
    CHECK(control->fast_manager()->owns_account("A"));
    CHECK(control->smart_runtime()->owns_account("B"));

    // Selections are validated in full before anything is published.
    sink->clear();
    for (const json& bad : {json{{"trading_date", date}, {"account_ids", json::array()}},
                            json{{"trading_date", date}, {"account_ids", json::array({"A", "missing"})}},
                            json{{"trading_date", date}, {"account_ids", json::array({"__ALL__", "missing"})}},
                            json{{"trading_date", "2026-02-30"}, {"account_ids", json::array({"A"})}},
                            json{{"trading_date", "2999-01-01"}, {"account_ids", json::array({"A"})}},
                            json{{"trading_date", "\"><script>alert(1)</script>"}, {"account_ids", json::array({"B"})}}}) {
        CAPTURE(bad.dump());
        CHECK(lmh.Post("/api/broker-orders", bad.dump(), "application/json")->status == 400);
    }
    CHECK(sink->count() == 0);
    auto all = lmh.Post("/api/broker-orders", json{{"trading_date", date}, {"account_ids", json::array({"__ALL__"})}}.dump(), "application/json");
    CHECK(all->status == 200);
    CHECK(sink->history().back().command["account_ids"] == json::array({"A", "B", "C"}));

    // A transport failure is reported without leaking details or claiming success.
    sink->clear();
    sink->fail_next("private transport detail");
    auto failed = lmh.Post("/api/broker-orders", body_json, "application/json");
    CHECK(failed->status == 503);
    CHECK(failed->body.find("private transport detail") == std::string::npos);
    CHECK(body(failed)["error"] == ng::i18n::t("en", "broker_orders_publish_failed"));
    CHECK(sink->count() == 0);

    // The form route keeps the redirect flow: success and failure both return to the page.
    auto form_ok = lmh.Post("/submit-broker-orders", "trading_date=" + date + "&account_ids=B&account_ids=A", "application/x-www-form-urlencoded");
    CHECK(form_ok->status == 303);
    CHECK(form_ok->get_header_value("Location").find("/admin-control?ok=") == 0);
    CHECK(sink->history().back().command["account_ids"] == json::array({"A", "B"}));
    auto form_bad = lmh.Post("/submit-broker-orders", "trading_date=2026-02-30&account_ids=A", "application/x-www-form-urlencoded");
    CHECK(form_bad->get_header_value("Location").find("/admin-control?err=") == 0);
    auto cross = lmh.Post("/api/broker-orders", httplib::Headers{{"Origin", "https://evil.example"}}, body_json, "application/json");
    CHECK(cross->status == 403);
    CHECK(sink->count() == 1);
}
