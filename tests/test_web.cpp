#include "web_fixture.h"

#include <set>

#include "orders/commands.h"

using ng::json;
using namespace ng::web;
using ngtest::body;
using ngtest::WebFixture;

TEST_CASE("HTTP redirector serves requests and preserves the path and query") {
    WebFixture f;
    httplib::Server port_probe;
    int redirect_port = port_probe.bind_to_any_port("127.0.0.1");
    REQUIRE(redirect_port > 0);
    port_probe.stop();

    ServerOptions options;
    options.host = "127.0.0.1";
    options.port = 0;
    options.threads = 2;
    options.redirect_host = "127.0.0.1";
    options.redirect_port = redirect_port;
    options.redirect_target_host = "example.test";
    Server server(*f.control, f.sessions, options);
    server.start();

    httplib::Client client("127.0.0.1", redirect_port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    auto get = client.Get("/login?next=/admin-control&lang=en");
    REQUIRE(get);
    CHECK(get->status == 308);
    CHECK(get->get_header_value("Location") == "https://example.test/login?next=/admin-control&lang=en");

    auto post = client.Post("/login", "", "application/x-www-form-urlencoded");
    REQUIRE(post);
    CHECK(post->status == 308);
    CHECK(post->get_header_value("Location") == "https://example.test/login");
    CHECK(f.sink->count() == 0);
}

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
    for (const char* api : {"/api/quotes", "/api/market-data", "/api/smart-buying", "/api/smart-selling", "/api/algo", "/api/health"}) {
        CAPTURE(std::string(api));
        CHECK(anon.Get(api)->status == 401);
    }
    auto legacy = anon.Post("/submit-order", "account_id=A&symbol=AAPL&side=BUY&shares=1", "application/x-www-form-urlencoded");
    CHECK(legacy->status == 303);
    CHECK(ng::starts_with(legacy->get_header_value("Location"), "/login?next="));
}

TEST_CASE("admin control and market data pages require the exact lmh user") {
    WebFixture f;
    // Anonymous visitors get the application shell, which shows the sign-in form.
    auto anon = f.client("");
    for (const char* path : {"/admin-control", "/market-data"}) {
        auto page = anon.Get(path);
        REQUIRE(page);
        CHECK(page->status == 200);
        CHECK(page->body.find("<div id=\"app\"") != std::string::npos);
    }
    for (const char* user : {"admin", "trader", "LMH", " lmh "}) {
        CAPTURE(std::string(user));
        auto c = f.client_for(user);
        for (const char* path : {"/admin-control", "/market-data"}) {
            auto page = c.Get(path);
            REQUIRE(page);
            CHECK(page->status == 403);
            CHECK(page->get_header_value("Cache-Control") == "no-store");
            CHECK(page->body.find("<div id=\"app\"") == std::string::npos);
        }
        json boot = body(c.Get("/api/bootstrap"));
        CHECK(boot["can_access_admin_control"] == false);
        CHECK(boot["can_access_market_data"] == false);
        // Trading-status control keeps its older, whitespace-tolerant check (_has_lmh_feature_access).
        CHECK(boot["can_manage_trading"] == (std::string(user) == " lmh "));
        // Everyone signed in keeps the ordinary pages and price lookups.
        CHECK(c.Get("/control-panel")->status == 200);
        CHECK(c.Get("/api/quotes?symbol=AAPL")->status == 200);
    }
    auto lmh = f.client_for("lmh");
    for (const char* path : {"/admin-control", "/market-data"}) {
        auto page = lmh.Get(path);
        REQUIRE(page);
        CHECK(page->status == 200);
        CHECK(page->body.find("<div id=\"app\"") != std::string::npos);
    }
    json boot = body(lmh.Get("/api/bootstrap"));
    CHECK(boot["can_access_admin_control"] == true);
    CHECK(boot["can_access_market_data"] == true);
    CHECK(boot["can_manage_trading"] == true);
    // The former Currency Conversion page and submission routes are gone.
    CHECK(lmh.Get("/currency-conversion")->status == 404);
    CHECK(lmh.Post("/submit-currency-conversion", "", "application/x-www-form-urlencoded")->status == 404);
    CHECK(lmh.Post("/api/fx", "{}", "application/json")->status == 404);
    auto get_submit = lmh.Get("/submit-broker-orders");
    CHECK(get_submit->status == 405);
    CHECK(get_submit->get_header_value("Allow") == "POST");
    CHECK(f.sink->count() == 0);
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
    CHECK(boot["market_data_symbols"] == json::array({"AAPL", "MSFT"}));
    CHECK(boot["accounts"][0]["id"] == "A");
    CHECK(boot["accounts"][0]["monitor"] == true);
    CHECK(boot["smart_selling_available"] == true);
    CHECK(boot["smart_buying_available"] == true);
    CHECK(boot["broker_orders_today"] == ng::orders::broker_orders_today());
    CHECK(boot["fast_trading"]["target_percentage_choices"] == json::array({10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 120, 150}));
    CHECK_FALSE(boot.contains("can_convert_currency"));
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
    f.upsert_quote("AAPL", 125.0, 120.0, 1000);
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
    CHECK(view["portfolio"]["total_market_value"] == 1125.0);
    CHECK(view["portfolio"]["market_value_complete"] == true);
    CHECK(view["metas"][0]["has_snapshot"] == true);
    CHECK(view["stock_quotes"]["AAPL"]["last"] == 125.0);

    // /api/quotes: complete live trade snapshots, independent of the depth feed (tests/test_stock_quotes.py).
    json quotes = body(c.Get("/api/quotes"));
    CHECK(quotes["rows"].size() == 2);
    const json& row = quotes["rows"]["AAPL"];
    CHECK(row["symbol"] == "AAPL");
    CHECK(row["prev_close"] == 120.0);
    CHECK(row["last"] == 125.0);
    CHECK(row["volume"] == 1000);
    CHECK(row["change"] == 5.0);
    CHECK(row["change_pct"].get<double>() == doctest::Approx(5.0 / 120 * 100));
    CHECK(row["asof_epoch"].is_number_integer());
    CHECK(row["short_interest"].is_null());
    CHECK(row["error"].is_null());
    CHECK(quotes["rows"]["MSFT"]["error"] == "no live quote yet");
    for (const char* key : {"prev_close", "last", "volume", "change", "change_pct"}) CHECK(quotes["rows"]["MSFT"][key].is_null());
    json one = body(c.Get("/api/quotes?symbol=%20aapl%20"));
    CHECK(one["rows"].size() == 1);
    CHECK(one["rows"]["AAPL"]["last"] == 125.0);
    json custom = body(c.Get("/api/quotes?symbol=nvda"));
    CHECK(custom["rows"].size() == 1);
    CHECK(custom["rows"]["NVDA"]["error"] == "no live quote yet");
    // Missing reference fields never invalidate the trade; a stale observation clears every value.
    f.upsert_quote("NVDA", 400.0);
    f.upsert_quote("MSFT", 126.0, 120.0, 2000, 31);
    quotes = body(c.Get("/api/quotes?symbol=NVDA"));
    CHECK(quotes["rows"]["NVDA"]["last"] == 400.0);
    CHECK(quotes["rows"]["NVDA"]["prev_close"].is_null());
    CHECK(quotes["rows"]["NVDA"]["change"].is_null());
    CHECK(quotes["rows"]["NVDA"]["volume"].is_null());
    CHECK(quotes["rows"]["NVDA"]["error"].is_null());
    quotes = body(c.Get("/api/quotes"));
    CHECK(quotes["rows"]["MSFT"]["error"] == "quote feed is stale");
    CHECK(quotes["rows"]["MSFT"]["asof_epoch"].is_number_integer());
    for (const char* key : {"prev_close", "last", "volume", "change", "change_pct"}) CHECK(quotes["rows"]["MSFT"][key].is_null());
    CHECK(quotes["rows"]["AAPL"]["last"] == 125.0);

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
    CHECK(health["feeds"]["own_orders"]["state"] == "starting");
    CHECK(health["algorithms"]["buying"].is_object());
}

TEST_CASE("market data windows are served to lmh only, validated and never cached") {
    WebFixture f;
    for (const char* user : {"admin", "trader", "LMH", " lmh "}) {
        CAPTURE(std::string(user));
        auto denied = f.client_for(user).Get("/api/market-data?symbol=AAPL");
        REQUIRE(denied);
        CHECK(denied->status == 403);
        CHECK(denied->get_header_value("Cache-Control") == "no-store");
        CHECK(body(denied)["error"] == ng::i18n::t("en", "md_forbidden"));
    }
    auto lmh = f.client_for("lmh");
    ng::StockQuote quote;
    quote.symbol = "AAPL";
    quote.last = 10.0;
    quote.observed_at = ng::wall_now();
    quote.trade_time = quote.observed_at - 1;
    quote.volume = 100;
    quote.last_size = 25;
    f.control->market_activity()->record_quote(quote);
    f.control->market_order_store()->update_feed(std::string("live"), std::nullopt);
    auto response = lmh.Get("/api/market-data?symbol=%20aapl%20");
    REQUIRE(response);
    CHECK(response->status == 200);
    CHECK(response->get_header_value("Cache-Control") == "no-store");
    json data = body(response);
    CHECK(data["ok"] == true);
    CHECK(data["symbol"] == "AAPL");
    CHECK(data["symbols"] == json::array({"AAPL", "MSFT"}));
    CHECK(data["windows"].size() == 4);
    CHECK(data["timezone"] == "America/New_York");
    CHECK(data["order_feed"] == "live");
    CHECK(data["quote_state"] == "live");
    CHECK(data["last_trade"]["quantity"] == 25);
    CHECK(data["last_trade"]["price"] == 10.0);
    CHECK(data["windows"][0]["our_complete"] == true);
    CHECK(data["windows"][0]["gross"] == 0.0);
    auto unconfigured = lmh.Get("/api/market-data?symbol=UNCONFIGURED");
    CHECK(unconfigured->status == 400);
    CHECK(unconfigured->get_header_value("Cache-Control") == "no-store");
    CHECK(body(lmh.Get("/api/market-data"))["symbol"] == "AAPL");
    CHECK(body(lmh.Get("/api/market-data?symbol=MSFT"))["last_trade"]["price"].is_null());
    // A skipped invalid order message makes our own figures incomplete even while the feed is live.
    f.control->market_order_store()->reject_message();
    data = body(lmh.Get("/api/market-data"));
    CHECK(data["order_feed"] == "incomplete");
    CHECK(data["windows"][0]["our_complete"] == false);
    CHECK(data["windows"][0]["gross"].is_null());
}

TEST_CASE("market data reports an unavailable service explicitly") {
    // Without the activity database (reporting not created) the dashboard is unavailable, not empty.
    ngtest::TempDir dir("web-no-reporting");
    ngtest::write_file(dir.file("accounts.json"), R"({"accounts": [{"string_id": "A", "numeric_id": 1, "broker": "Tiger", "trading_medium": "API"}]})");
    ngtest::write_file(dir.file("config.json"), R"({"accounts_file": "./accounts.json", "symbols": ["AAPL"],
        "auth": {"users": {"admin": {"password": "a"}, "trader": {"password": "b"}, "lmh": {"password": "c"}}},
        "kafka": {"bootstrap_servers": "127.0.0.1:1", "market_data_bootstrap_servers": "127.0.0.1:1"}})");
    ng::AppConfig config = ng::load_config(dir.file("config.json"));
    ControlCenter control(config, ng::load_account_metas(config), ng::load_symbols(config));
    SessionStore sessions;
    ServerOptions options;
    options.host = "127.0.0.1";
    options.port = 0;
    Server server(control, sessions, options);
    server.start();
    sessions.insert_for_test("lmh-token", "lmh");
    httplib::Client c("127.0.0.1", server.port());
    c.set_default_headers({{"Cookie", "auth_token=lmh-token"}});
    auto response = c.Get("/api/market-data");
    REQUIRE(response);
    CHECK(response->status == 503);
    CHECK(body(response)["error"] == ng::i18n::t("en", "md_unavailable"));
    CHECK(body(c.Get("/api/bootstrap"))["smart_buying_available"] == false);
    server.stop();
    control.shutdown();
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
    auto lmh = f.client("lmh-token");
    auto status_forbidden = c.Post("/api/trading-status", json{{"account_id", "A"}, {"trading_enabled", false}}.dump(), "application/json");
    CHECK(status_forbidden->status == 403);
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

TEST_CASE("algorithm sessions share the server and block manual orders on their accounts") {
    WebFixture f;
    f.upsert_account("A", 100000, 1000);
    f.upsert_account("B", 100000, 1000);
    f.upsert_book("XYZ", 10, 10.01);
    auto c = f.client();
    json start = body(c.Post("/api/submit-algo", json{{"trading_mode", "E"}, {"symbol", "AAPL"}, {"fast_trading_config", {{"price_limit", 10}, {"account_ids", json::array({"A"})}, {"book_levels", 1}, {"target_percentage", 100}, {"cycle_seconds", 5}, {"test_mode", true}, {"total_target_position", "2500"}}}}.dump(), "application/json"));
    REQUIRE(start["ok"] == true);
    CHECK(start["strategy_key"] == "E:AAPL");
    CHECK(start["command"]["type"] == "START_ALGO_TRADING");
    CHECK(start["command"]["fast_trading_total_target_position"] == 2500);
    // The same account cannot join a second session, in any mode.
    auto again = c.Post("/api/submit-algo", json{{"trading_mode", "F"}, {"symbol", "MSFT"}, {"fast_trading_price_limit", 10}, {"fast_trading_account_ids", json::array({"A"})}, {"fast_trading_book_levels", 1}, {"fast_trading_target_percentage", 100}, {"fast_trading_cycle_seconds", 5}}.dump(), "application/json");
    CHECK(again->status == 500);  // like the Python API: a start conflict is not an input validation error
    CHECK(body(again)["error"].get<std::string>().find("Account(s) A already belong to an algorithm session") == 0);
    auto smart_blocked = c.Post("/api/smart-selling", json{{"account_ids", json::array({"A"})}, {"symbol", "AAPL"}, {"deadline", nullptr}}.dump(), "application/json");
    CHECK(smart_blocked->status == 400);
    auto bad_target = c.Post("/api/submit-algo", json{{"trading_mode", "E"}, {"symbol", "MSFT"}, {"fast_trading_price_limit", 10}, {"fast_trading_account_ids", json::array({"B"})}, {"fast_trading_book_levels", 1}, {"fast_trading_target_percentage", 100}, {"fast_trading_cycle_seconds", 5}, {"total_target_position", "1.5"}}.dump(), "application/json");
    CHECK(bad_target->status == 400);
    CHECK(body(bad_target)["error"] == ng::i18n::t("en", "fast_total_target_position_invalid"));
    // A disjoint account starts a second, concurrent session with an oversized target.
    json second = body(c.Post("/api/submit-algo", json{{"trading_mode", "F"}, {"symbol", "MSFT"}, {"fast_trading_price_limit", 10}, {"fast_trading_account_ids", json::array({"B"})}, {"fast_trading_book_levels", 1}, {"fast_trading_target_percentage", 150}, {"fast_trading_cycle_seconds", 5}, {"fast_trading_test_mode", true}}.dump(), "application/json"));
    REQUIRE(second["ok"] == true);
    CHECK(second["strategy_key"] == "F:MSFT");
    CHECK(second["command"]["fast_trading_target_percentage"] == 150);
    auto owned = c.Post("/api/submit-order", json{{"account_id", "A"}, {"symbol", "MSFT"}, {"side", "BUY"}, {"shares", 1}}.dump(), "application/json");
    CHECK(owned->status == 500);
    CHECK(body(owned)["error"].get<std::string>().find("owns this account") != std::string::npos);
    json algo = body(c.Get("/api/algo"));
    CHECK(algo["active"] == true);
    REQUIRE(algo["fast"].size() == 2);
    std::set<std::string> session_ids;
    for (const auto& session : algo["fast"]) session_ids.insert(session["session_id"].get<std::string>());
    CHECK(session_ids == std::set<std::string>{start["command"]["command_id"].get<std::string>(),
                                               second["command"]["command_id"].get<std::string>()});
    json stop = body(c.Post("/api/algo/stop", json{{"reason", "test"}}.dump(), "application/json"));
    CHECK(stop["ok"] == true);
    CHECK(stop["fast_stopped"] == 2);
    CHECK(stop["buying_stopped"] == 0);
    CHECK(f.sink->history().back().command["type"] == "STOP_ALGO_TRADING");
    CHECK(f.sink->history().back().command["reason"] == "test");
    CHECK(f.wait_until([&] { return !f.control->any_algorithm_active(); }));

    // Smart selling start/stop through the API.
    f.upsert_account("B", 100000, 1000);
    json smart = body(c.Post("/api/smart-selling", json{{"account_ids", json::array({"B"})}, {"symbol", "AAPL"}, {"deadline", nullptr}, {"mode", "ultra"}, {"disable_child_cap", true}}.dump(), "application/json"));
    REQUIRE(smart["ok"] == true);
    std::string parent_id = smart["parent"]["parent_id"].get<std::string>();
    CHECK(smart["parent"]["quantity"] == "1000");
    CHECK(smart["parent"]["mode"] == "ultra");
    CHECK(smart["parent"]["participation"] == "0.30");
    CHECK(smart["parent"]["disable_child_cap"] == true);
    CHECK(smart["parent"]["child_cap"].is_null());
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

// tests/test_smart_buying.py::test_authenticated_same_origin_api_starts_and_stops_mode_h
TEST_CASE("authenticated same-origin API starts and stops mode H") {
    WebFixture f;
    f.upsert_account("A", 10000, 0);
    f.upsert_book("XYZ", 19.99, 20);
    std::string payload = json{{"symbol", "XYZ"}, {"account_ids", json::array({"A"})}, {"deadline", nullptr}}.dump();
    CHECK(f.client("").Post("/api/smart-buying", payload, "application/json")->status == 401);
    auto c = f.client();
    auto cross = c.Post("/api/smart-buying", httplib::Headers{{"Origin", "https://elsewhere.invalid"}}, payload, "application/json");
    CHECK(cross->status == 403);
    CHECK(f.control->buying_runtime()->list_parents().empty());
    auto created = c.Post("/api/smart-buying", payload, "application/json");
    REQUIRE(created);
    CHECK(created->status == 200);
    json parent = body(created)["parent"];
    CHECK(parent["participation"] == "0.08");
    CHECK(parent["trading_mode"] == "H");
    CHECK(parent["maximum_price"].is_null());
    CHECK(parent["accounts"]["A"]["cash_budget"] == "10000.0");
    CHECK(parent["accounts"]["A"]["spent"] == "0");
    CHECK(parent["quantity"] == "500");  // $10,000 of cash at the $20 best ask
    json listed = body(c.Get("/api/smart-buying"));
    CHECK(listed["parents"][0]["parent_id"] == parent["parent_id"]);
    CHECK(listed["available"] == true);
    // The account is now owned by the session: no other mode and no manual order may use it.
    auto fast = c.Post("/api/submit-algo", json{{"trading_mode", "E"}, {"symbol", "AAPL"}, {"fast_trading_price_limit", 10}, {"fast_trading_account_ids", json::array({"A"})}, {"fast_trading_book_levels", 1}, {"fast_trading_target_percentage", 100}, {"fast_trading_cycle_seconds", 5}}.dump(), "application/json");
    CHECK(fast->status == 500);
    CHECK(body(fast)["error"].get<std::string>().find("already belong to an algorithm session") != std::string::npos);
    CHECK(c.Post("/api/submit-order", json{{"account_id", "A"}, {"symbol", "XYZ"}, {"side", "BUY"}, {"shares", 1}}.dump(), "application/json")->status == 500);
    json algo = body(c.Get("/api/algo"));
    CHECK(algo["active"] == true);
    CHECK(algo["buying"]["parents"].size() == 1);
    auto invalid = c.Post("/api/smart-buying", json{{"symbol", "XYZ"}, {"account_ids", json::array({"B"})}, {"mode", "ultra"}}.dump(), "application/json");
    CHECK(invalid->status == 400);
    CHECK(c.Post("/api/smart-buying/unknown/stop", "", "application/json")->status == 404);
    std::string parent_id = parent["parent_id"].get<std::string>();
    auto stopped = c.Post("/api/smart-buying/" + parent_id + "/stop", "", "application/json");
    CHECK(stopped->status == 200);
    CHECK(body(stopped)["parent"]["state"] == "STOPPED");
    CHECK(body(c.Get("/api/smart-buying/" + parent_id + "/events"))["events"].size() >= 2);
    CHECK_FALSE(f.control->any_algorithm_active());
    // Stop-all reports Smart buying sessions separately.
    f.upsert_account("A", 10000, 0);
    REQUIRE(c.Post("/api/smart-buying", payload, "application/json")->status == 200);
    json stop_all = body(c.Post("/api/algo/stop", "{}", "application/json"));
    CHECK(stop_all["buying_stopped"] == 1);
    CHECK(stop_all["message"].get<std::string>().find("Smart buying") != std::string::npos);
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
