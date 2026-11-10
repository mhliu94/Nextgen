// One-session-per-account rule across modes E/F/G/H and the manual-order guards
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

const char* kConflict = "already belong to an algorithm session";

struct SessionFixture : WebFixture {
    json smart_settings = {{"account_ids", json::array({"B"})}, {"symbol", "XYZ"}, {"deadline", nullptr}};

    SessionFixture()
        : WebFixture(R"({"accounts": [
            {"string_id": "A", "numeric_id": 1, "broker": "Tiger", "trading_medium": "API"},
            {"string_id": "B", "numeric_id": 2, "broker": "Tiger", "trading_medium": "API"},
            {"string_id": "C", "numeric_id": 3, "broker": "Tiger", "trading_medium": "API"}]})") {
        for (const char* account : {"A", "B", "C"}) upsert_account(account, 10000, 1000, true, "XYZ");
    }
    json fast(const std::string& command_id, const std::string& mode, const std::string& symbol, std::vector<std::string> accounts) {
        return fast_start(command_id, mode, symbol, std::move(accounts), false);
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

TEST_CASE_FIXTURE(SessionFixture, "an account in one session cannot join another in any mode") {
    control->start_fast_strategy(fast("buy", "E", "AAPL", {"A"}));
    json parent = control->start_smart_parent(smart_settings);
    CHECK(conflicts([&] { control->start_fast_strategy(fast("sell", "F", "MSFT", {"C", "A"})); }));
    CHECK(conflicts([&] { control->start_fast_strategy(fast("again", "E", "AAPL", {"B"})); }));
    json both = smart_settings;
    both["account_ids"] = json::array({"C", "A"});
    CHECK(conflicts([&] { control->start_smart_parent(both); }));
    json other_symbol = smart_settings;
    other_symbol["symbol"] = "OTHER";
    CHECK(conflicts([&] { control->start_smart_parent(other_symbol); }));
    json buying = {{"account_ids", json::array({"B"})}, {"symbol", "XYZ"}, {"deadline", nullptr}};
    CHECK(conflicts([&] { control->start_smart_buying_parent(buying); }));
    CHECK_FALSE(control->fast_manager()->owns_account("C"));
    CHECK_FALSE(control->smart_runtime()->owns_account("C"));
    // A released account is available to every mode again.
    control->fast_manager()->stop_all();
    REQUIRE(wait_until([&] { return !control->fast_manager()->has_active_session(); }));
    json only_a = smart_settings;
    only_a["account_ids"] = json::array({"A"});
    control->start_smart_parent(only_a);
    control->stop_smart_parent(parent["parent_id"].get<std::string>());
    control->start_fast_strategy(fast("buy-b", "E", "AAPL", {"B"}));
    CHECK(control->fast_manager()->owns_account("B"));
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

TEST_CASE_FIXTURE(SessionFixture, "fast handoff proceeds while smart selling owns other accounts") {
    control->start_fast_strategy(fast_command());
    control->start_smart_parent(smart_settings);
    json command = {{"type", "LIMIT_ORDER_FOK"}, {"account_id", "A"}, {"symbol", "AAPL"}, {"command_id", "child-1"}};
    control->publish_fast_trading_command(command, "A");
    REQUIRE(sink->count() == 1);
    CHECK(sink->history()[0].key == "A");
    // Ownership is exclusive: a Smart selling claim on the same account halts the handoff. (The
    // runtime never admits such a parent; the engine is driven directly to model the conflict.)
    sink->clear();
    control->smart_runtime()->engine().start_parent(
        json{{"account_id", "A"}, {"symbol", "ZZZ"}, {"quantity", "1000"}, {"mode", "urgent"}, {"deadline", nullptr},
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
