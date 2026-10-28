#include <doctest.h>

#include <httplib.h>

#include <chrono>
#include <thread>

#include "core/config.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "core/i18n.h"
#include "algo/smart_selling/engine.h"
#include "test_helpers.h"
#include "web/control.h"
#include "web/server.h"
#include "web/sessions.h"

using ng::json;
using namespace ng::web;

namespace {

struct WebFixture {
    ngtest::TempDir dir{"web"};
    ng::AppConfig config;
    std::unique_ptr<ControlCenter> control;
    SessionStore sessions;
    std::unique_ptr<Server> server;
    RecordingCommandSink* sink = nullptr;

    WebFixture() {
        ngtest::write_file(dir.file("accounts.json"), R"({"accounts": [
          {"string_id": "A", "numeric_id": 1, "broker": "Tiger", "trading_medium": "API", "monitor": true, "ip_address": "1.1.1.1"},
          {"string_id": "B", "numeric_id": 2, "broker": "GigaMoney", "trading_medium": "EMULATOR"}]})");
        ngtest::write_file(dir.file("hist.csv"), "symbol,trade_date,close\nAAPL,2026-10-24,100\n");
        ngtest::write_file(dir.file("config.json"), R"({
          "accounts_file": "./accounts.json", "symbols": ["AAPL", "MSFT"],
          "auth": {"users": {"admin": {"password": "admin-pw"}, "trader": {"password": "trader-pw"}, "lmh": {"password": "lmh-pw"}}},
          "server": {"host": "127.0.0.1", "port": 0, "ssl_enabled": false},
          "kafka": {"bootstrap_servers": "127.0.0.1:1", "market_data_bootstrap_servers": "127.0.0.1:1"},
          "market_data": {"historical_prices_csv": "./hist.csv"},
          "smart_selling": {"state_db": "./smart.sqlite3"}})");
        unsetenv("UI_SSL_ENABLED");
        config = ng::load_config(dir.file("config.json"));
        config.operations_log_dir = dir.file("logs");
        control = std::make_unique<ControlCenter>(config, ng::load_account_metas(config), ng::load_symbols(config));
        auto recording = std::make_unique<RecordingCommandSink>();
        sink = recording.get();
        control->set_command_sink(std::move(recording));
        control->set_operations_log(std::make_unique<ng::OperationsLog>(dir.file("logs"), ng::wall_now(), [this] { return control->served_snapshots(); }));
        control->create_fast_trading_manager();
        control->create_smart_selling_runtime(std::make_shared<ng::smart::SmartSellingEngine>(":memory:"));
        control->smart_runtime()->set_feed_states_for_test({{"orders", "live"}, {"volume", "live"}});
        control->start_background();
        ServerOptions options;
        options.host = "127.0.0.1";
        options.port = 0;
        options.threads = 8;
        server = std::make_unique<Server>(*control, sessions, options);
        server->start();
        sessions.insert_for_test("trader-token", "trader");
        sessions.insert_for_test("lmh-token", "lmh");
    }
    ~WebFixture() {
        server->stop();
        control->shutdown();
    }
    httplib::Client client(const std::string& token = "trader-token", const std::string& lang = "") {
        httplib::Client c("127.0.0.1", server->port());
        c.set_connection_timeout(5, 0);
        c.set_read_timeout(5, 0);
        std::string cookie;
        if (!token.empty()) cookie = "auth_token=" + token;
        if (!lang.empty()) cookie += (cookie.empty() ? "" : "; ") + std::string("lang=") + lang;
        if (!cookie.empty()) c.set_default_headers({{"Cookie", cookie}});
        return c;
    }
    void upsert_account(const std::string& id, double usd, double aapl_qty, bool trading = true) {
        ng::AccountSnapshot s;
        s.account_id = id;
        s.cash = usd;
        s.cash_by_currency["USD"] = usd;
        s.available_cash_by_currency["USD"] = usd;
        if (aapl_qty) s.positions.push_back(ng::Position{"AAPL", aapl_qty, 90.0, aapl_qty});
        s.ts = ng::iso_utc_now();
        s.trading_enabled = trading;
        control->account_store().upsert(s);
    }
    void upsert_book(const std::string& symbol, double bid, double ask) {
        json payload = {{"symbol", symbol}, {"update_time", 1700000000000000000LL},
                        {"bid_side", {{"levels", json::array({{{"prc", bid}, {"qty", 1000}}, {{"prc", bid - 1}, {"qty", 500}}})}}},
                        {"offer_side", {{"levels", json::array({{{"prc", ask}, {"qty", 800}}})}}}};
        control->market_store().upsert(*ng::quote_row_from_price_book(payload));
        control->market_store().upsert_book(*ng::order_book_from_price_book(payload));
    }
};

json body(const httplib::Result& res) {
    REQUIRE(res);
    return json::parse(res->body, nullptr, false);
}

}  // namespace

TEST_CASE("pages are served and APIs require authentication") {
    WebFixture f;
    auto anon = f.client("");
    auto page = anon.Get("/account-details");
    REQUIRE(page);
    CHECK(page->status == 200);
    CHECK(page->body.find("<div id=\"app\"") != std::string::npos);
    CHECK(anon.Get("/static/app.js")->status == 200);
    CHECK(anon.Get("/static/../CMakeLists.txt")->status == 404);
    auto root = anon.Get("/");
    CHECK(root->status == 302);
    CHECK(root->get_header_value("Location") == "/account-details");
    auto accounts = anon.Get("/api/accounts");
    CHECK(accounts->status == 401);
    CHECK(body(accounts)["error"] == "Unauthorized");
    auto session = anon.Get("/api/session");
    CHECK(session->status == 401);
    CHECK(body(session)["authenticated"] == false);
    CHECK(anon.Get("/api/stream?topics=accounts")->status == 401);
    auto legacy = anon.Post("/submit-order", "account_id=A&symbol=AAPL&side=BUY&shares=1", "application/x-www-form-urlencoded");
    CHECK(legacy->status == 303);
    CHECK(ng::starts_with(legacy->get_header_value("Location"), "/login?next="));
}

TEST_CASE("login, session and logout") {
    WebFixture f;
    auto anon = f.client("");
    auto bad = anon.Post("/login", httplib::Headers{{"Accept", "application/json"}}, "username=trader&password=wrong", "application/x-www-form-urlencoded");
    CHECK(bad->status == 401);
    CHECK(body(bad)["error"] == ng::i18n::t("en", "login_failed"));
    auto bad_form = anon.Post("/login", "username=trader&password=wrong", "application/x-www-form-urlencoded");
    CHECK(bad_form->status == 303);
    CHECK(bad_form->get_header_value("Location").find("err=") != std::string::npos);
    auto ok = anon.Post("/login", httplib::Headers{{"Accept", "application/json"}}, "username=trader&password=trader-pw&next=/control-panel", "application/x-www-form-urlencoded");
    REQUIRE(ok->status == 200);
    std::string cookie = ok->get_header_value("Set-Cookie");
    CHECK(cookie.find("auth_token=") == 0);
    CHECK(cookie.find("HttpOnly") != std::string::npos);
    std::string token = cookie.substr(11, cookie.find(';') - 11);
    auto authed = f.client(token);
    auto session = authed.Get("/api/session");
    CHECK(session->status == 200);
    CHECK(body(session)["user"] == "trader");
    CHECK(body(session)["privileged"] == false);
    auto boot = body(authed.Get("/api/bootstrap"));
    CHECK(boot["symbols"] == json::array({"AAPL", "MSFT"}));
    CHECK(boot["accounts"][0]["id"] == "A");
    CHECK(boot["accounts"][0]["monitor"] == true);
    CHECK(boot["smart_selling_available"] == true);
    CHECK(authed.Post("/api/logout", "", "application/json")->status == 200);
    CHECK(authed.Get("/api/session")->status == 401);
    auto redirect_form = anon.Post("/login", "username=lmh&password=lmh-pw&next=/trading-status", "application/x-www-form-urlencoded");
    CHECK(redirect_form->status == 303);
    CHECK(redirect_form->get_header_value("Location") == "/trading-status");
    auto lang = anon.Get("/set-lang?lang=zh&next=/market-data");
    CHECK(lang->status == 302);
    CHECK(lang->get_header_value("Set-Cookie").find("lang=zh") == 0);
    CHECK(lang->get_header_value("Location") == "/market-data");
}

TEST_CASE("account and market views") {
    WebFixture f;
    f.upsert_account("B", 300, 7);
    f.upsert_account("A", 1250.5, 2);
    f.upsert_account("UNKNOWN", 999, 0);
    f.upsert_book("AAPL", 99, 101);
    ng::StockQuote quote{"AAPL", 125.0, 1700000000.0, 1700000001.0, false, false, std::nullopt};
    f.control->market_store().upsert_stock_quote(quote);
    auto c = f.client();
    json accounts = body(c.Get("/api/accounts"));
    REQUIRE(accounts["accounts"].size() == 2);
    CHECK(accounts["accounts"][0]["account_id"] == "A");
    CHECK(accounts["accounts"][0]["account_num_id"] == 1);
    CHECK(accounts["accounts"][1]["account_id"] == "B");
    CHECK(accounts["source"] == "kafka");
    json view = body(c.Get("/api/accounts/view"));
    CHECK(view["portfolio"]["cash"]["USD"] == 1550.5);
    REQUIRE(view["portfolio"]["positions"].size() == 1);
    CHECK(view["portfolio"]["positions"][0]["qty"] == 9.0);
    CHECK(view["portfolio"]["positions"][0]["last"] == 125.0);
    CHECK(view["portfolio"]["positions"][0]["market_value"] == 1125.0);
    CHECK(view["portfolio"]["positions"][0]["account_count"] == 2);
    CHECK(view["portfolio"]["included_accounts"] == 2);
    CHECK(view["metas"][0]["has_snapshot"] == true);
    CHECK(view["stock_quotes"]["AAPL"]["last"] == 125.0);
    json market = body(c.Get("/api/market-data"));
    CHECK(market["rows"]["AAPL"]["last"] == 100.0);
    CHECK(market["rows"]["AAPL"]["prev_close"] == 100.0);
    CHECK(market["rows"]["AAPL"]["change"] == 0.0);
    CHECK(market["rows"]["MSFT"]["error"] == "no data yet");
    json one = body(c.Get("/api/market-data?symbol=aapl"));
    CHECK(one["rows"].size() == 1);
    json insights = body(c.Get("/api/market-insights?symbol=AAPL&depth=1"));
    CHECK(insights["bids"].size() == 1);
    CHECK(insights["bids"][0]["price"] == 99.0);
    CHECK(insights["depth_limit"] == 1);
    CHECK(insights["symbols"] == json::array({"AAPL", "MSFT"}));
    auto bad = c.Get("/api/market-insights?symbol=");
    CHECK(bad->status == 400);
    json health = body(c.Get("/api/health"));
    CHECK(health["ok"] == true);
    CHECK(health["configured_accounts"] == 2);
    CHECK(health["accounts_with_data"] == 2);
}

TEST_CASE("order APIs validate, localize and publish once per account") {
    WebFixture f;
    f.upsert_account("A", 100000, 50);
    f.upsert_account("B", 100000, 0);
    f.upsert_book("AAPL", 99, 101);
    auto c = f.client();
    json market = body(c.Post("/api/orders/market", json{{"account_id", "A"}, {"symbol", "aapl"}, {"side", "BUY"}, {"shares", "100"}}.dump(), "application/json"));
    CHECK(market["ok"] == true);
    REQUIRE(f.sink->count() == 1);
    json published = f.sink->history()[0].command;
    CHECK(published["type"] == "MARKET_ORDER");
    CHECK(published["symbol"] == "AAPL");
    CHECK(published["qty_shares"] == 100);
    CHECK(f.sink->history()[0].key == "A");
    auto zh = f.client("trader-token", "zh");
    auto invalid = zh.Post("/api/orders/market", json{{"account_id", "Z"}, {"symbol", "AAPL"}, {"side", "BUY"}, {"shares", "1"}}.dump(), "application/json");
    CHECK(invalid->status == 400);
    CHECK(body(invalid)["error"] == ng::i18n::t("zh", "invalid_account"));
    json compat = body(c.Post("/api/submit-order", json{{"account_id", "A"}, {"symbol", "AAPL"}, {"side", "SELL"}, {"dollar_amount", 2500}}.dump(), "application/json"));
    CHECK(compat["ok"] == true);
    CHECK(compat["command"]["notional_usd"] == 2500.0);
    json quick = body(c.Post("/api/orders/quick", json{{"account_ids", json::array({"A", "B"})}, {"symbol", "AAPL"}, {"side", "BUY"}, {"dollar_amount", ""}}.dump(), "application/json"));
    CHECK(quick["ok"] == true);
    CHECK(f.sink->count() == 4);
    CHECK(f.sink->history()[2].command["qty_shares"] == 100);  // 10000 / 100 mid
    json limit = body(c.Post("/api/orders/limit", json{{"account_ids", json::array({"A"})}, {"symbol", "AAPL"}, {"side", "SELL"}, {"shares", "10"}, {"limit_price", ""}, {"through_market_pct", "10"}}.dump(), "application/json"));
    CHECK(limit["ok"] == true);
    CHECK(f.sink->history().back().command["limit_price"] == 90.0);
    json cancel = body(c.Post("/api/orders/cancel", json{{"account_ids", json::array({"__ALL__"})}, {"symbol", ""}}.dump(), "application/json"));
    CHECK(cancel["ok"] == true);
    auto hist = f.sink->history();
    CHECK(hist[hist.size() - 1].command["type"] == "CANCEL_OPEN_ORDERS");
    CHECK(hist[hist.size() - 2].command["type"] == "CANCEL_OPEN_ORDERS");
    CHECK_FALSE(hist.back().command.contains("symbol"));
    auto quick_form = c.Post("/api/account-orders/quick-market", "account_id=A&symbol=aapl&side=BUY&shares=5", "application/x-www-form-urlencoded");
    CHECK(quick_form->status == 200);
    CHECK(body(quick_form)["command_id"].get<std::string>().rfind("cmd_", 0) == 0);
    auto blank = c.Post("/api/account-orders/limit", "account_id=A&symbol=&side=BUY&shares=5&limit_price=1", "application/x-www-form-urlencoded");
    CHECK(blank->status == 400);
    CHECK(body(blank)["error"] == ng::i18n::t("en", "invalid_symbol"));
    auto two_accounts = c.Post("/api/account-orders/quick-market", "account_id=A&account_id=B&symbol=AAPL&side=BUY&shares=5", "application/x-www-form-urlencoded");
    CHECK(two_accounts->status == 400);
    auto forbidden = c.Post("/api/fx", json{{"account_id", "A"}, {"source_currency", "HKD"}, {"target_currency", "USD"}, {"source_amount", 10}}.dump(), "application/json");
    CHECK(forbidden->status == 403);
    auto lmh = f.client("lmh-token");
    json fx = body(lmh.Post("/api/fx", json{{"account_id", "A"}, {"source_currency", "HKD"}, {"target_currency", "USD"}, {"source_amount", 10}}.dump(), "application/json"));
    CHECK(fx["ok"] == true);
    CHECK(f.sink->history().back().command["type"] == "CURRENCY_CONVERSION");
    json off = body(lmh.Post("/api/trading-status", json{{"disable_all", true}, {"trading_enabled", false}}.dump(), "application/json"));
    CHECK(off["ok"] == true);
    auto h2 = f.sink->history();
    CHECK(h2[h2.size() - 1].command["type"] == "SET_TRADING_ENABLED");
    CHECK(h2[h2.size() - 1].command["trading_enabled"] == false);
    CHECK(h2[h2.size() - 2].command["type"] == "SET_TRADING_ENABLED");
    auto cross = c.Post("/api/orders/market", httplib::Headers{{"Origin", "https://evil.example"}}, json{{"account_id", "A"}, {"symbol", "AAPL"}, {"side", "BUY"}, {"shares", "1"}}.dump(), "application/json");
    CHECK(cross->status == 403);
    f.sink->fail_next("broker down");
    auto failed = c.Post("/api/submit-order", json{{"account_id", "A"}, {"symbol", "AAPL"}, {"side", "BUY"}, {"shares", 1}}.dump(), "application/json");
    CHECK(failed->status == 500);
    CHECK(body(failed)["error"] == "broker down");
    json legacy_ok = json::object();
    auto legacy = c.Post("/submit-quick-order", "account_ids=A&symbol=AAPL&side=BUY&dollar_amount=5000", "application/x-www-form-urlencoded");
    CHECK(legacy->status == 303);
    CHECK(legacy->get_header_value("Location").find("/control-panel?ok=") == 0);
    auto legacy_err = c.Post("/submit-quick-order", "symbol=AAPL&side=BUY", "application/x-www-form-urlencoded");
    CHECK(legacy_err->get_header_value("Location").find("/control-panel?err=") == 0);
    json commands = body(c.Get("/api/commands"));
    CHECK(commands["commands"].size() == f.sink->count());
}

TEST_CASE("algorithm sessions are exclusive and block manual orders on owned accounts") {
    WebFixture f;
    f.upsert_account("A", 100000, 1000);
    f.upsert_account("B", 100000, 1000);
    f.upsert_book("XYZ", 10, 10.01);
    auto c = f.client();
    json start = body(c.Post("/api/submit-algo", json{{"trading_mode", "E"}, {"symbol", "AAPL"}, {"fast_trading_config", {{"price_limit", 10}, {"account_ids", json::array({"A"})}, {"book_levels", 1}, {"target_percentage", 100}, {"cycle_seconds", 5}, {"test_mode", true}}}}.dump(), "application/json"));
    REQUIRE(start["ok"] == true);
    CHECK(start["strategy_key"] == "E:AAPL");
    CHECK(start["command"]["type"] == "START_ALGO_TRADING");
    auto again = c.Post("/api/submit-algo", json{{"trading_mode", "F"}, {"symbol", "MSFT"}, {"fast_trading_price_limit", 10}, {"fast_trading_account_ids", json::array({"B"})}, {"fast_trading_book_levels", 1}, {"fast_trading_target_percentage", 100}, {"fast_trading_cycle_seconds", 5}}.dump(), "application/json");
    CHECK(again->status == 500);
    CHECK(body(again)["error"].get<std::string>().find("Stop the existing") == 0);
    auto smart_blocked = c.Post("/api/smart-selling", json{{"account_ids", json::array({"B"})}, {"symbol", "XYZ"}, {"deadline", nullptr}}.dump(), "application/json");
    CHECK(smart_blocked->status == 400);
    auto owned = c.Post("/api/submit-order", json{{"account_id", "A"}, {"symbol", "MSFT"}, {"side", "BUY"}, {"shares", 1}}.dump(), "application/json");
    CHECK(owned->status == 500);
    CHECK(body(owned)["error"].get<std::string>().find("owns this account") != std::string::npos);
    auto other = c.Post("/api/submit-order", json{{"account_id", "B"}, {"symbol", "MSFT"}, {"side", "BUY"}, {"shares", 1}}.dump(), "application/json");
    CHECK(other->status == 200);
    json algo = body(c.Get("/api/algo"));
    CHECK(algo["active"] == true);
    CHECK(algo["fast"].size() == 1);
    json stop = body(c.Post("/api/algo/stop", json{{"reason", "test"}}.dump(), "application/json"));
    CHECK(stop["ok"] == true);
    CHECK(stop["fast_stopped"] == 1);
    CHECK(f.sink->history().back().command["type"] == "STOP_ALGO_TRADING");
    CHECK(f.sink->history().back().command["reason"] == "test");
    for (int i = 0; i < 100 && f.control->any_algorithm_active(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(f.control->any_algorithm_active());

    // Smart selling start/stop through the API.
    f.upsert_account("B", 100000, 1000);
    json smart = body(c.Post("/api/smart-selling", json{{"account_ids", json::array({"B"})}, {"symbol", "AAPL"}, {"deadline", nullptr}}.dump(), "application/json"));
    REQUIRE(smart["ok"] == true);
    std::string parent_id = smart["parent"]["parent_id"].get<std::string>();
    CHECK(smart["parent"]["quantity"] == "1000");
    json status = body(c.Get("/api/smart-selling"));
    CHECK(status["parents"].size() == 1);
    CHECK(status["parents"][0]["market_state"] == "UNKNOWN");
    auto blocked = c.Post("/api/submit-order", json{{"account_id", "B"}, {"symbol", "AAPL"}, {"side", "SELL"}, {"shares", 1}}.dump(), "application/json");
    CHECK(blocked->status == 500);
    CHECK(c.Post("/api/smart-selling/nope/stop", "", "application/json")->status == 404);
    json stopped = body(c.Post("/api/smart-selling/" + parent_id + "/stop", "", "application/json"));
    CHECK(stopped["ok"] == true);
    CHECK(stopped["parent"]["state"] == "STOPPED");
    json events = body(c.Get("/api/smart-selling/" + parent_id + "/events?limit=5"));
    CHECK(events["events"].size() >= 2);
    // Stop-all keeps working when the notification transport fails.
    f.upsert_account("B", 100000, 1000);
    body(c.Post("/api/smart-selling", json{{"account_ids", json::array({"B"})}, {"symbol", "AAPL"}, {"deadline", nullptr}}.dump(), "application/json"));
    f.sink->fail_next("transport unavailable");
    json stop_all = body(c.Post("/api/algo/stop", "{}", "application/json"));
    CHECK(stop_all["ok"] == true);
    CHECK(stop_all["smart_stopped"] == 1);
    CHECK(stop_all["warning"] == ng::i18n::t("en", "algo_stop_notification_failed"));
    CHECK_FALSE(f.control->smart_runtime()->has_active_session());
}

TEST_CASE("server-sent events deliver the initial snapshot and updates") {
    WebFixture f;
    f.upsert_account("A", 100, 5);
    auto c = f.client();
    std::string received;
    int events = 0;
    std::thread updater([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        f.upsert_account("A", 250, 8);
    });
    auto res = c.Get("/api/stream?topics=accounts", httplib::Headers{}, [&](const char* data, size_t len) {
        received.append(data, len);
        events = 0;
        std::size_t pos = 0;
        while ((pos = received.find("event: accounts", pos)) != std::string::npos) {
            ++events;
            pos += 15;
        }
        return events < 2;
    });
    updater.join();
    CHECK(events >= 2);
    CHECK(received.find("\"cash\":100.0") != std::string::npos);
    CHECK(received.find("\"cash\":250.0") != std::string::npos);
    CHECK(received.find("data: {") != std::string::npos);
    // Legacy alias keeps the account-details path.
    std::string alias;
    c.Get("/api/account-details/stream", httplib::Headers{}, [&](const char* data, size_t len) {
        alias.append(data, len);
        return alias.find("\n\n") == std::string::npos;
    });
    CHECK(alias.find("event: accounts") != std::string::npos);
}
