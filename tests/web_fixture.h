// Shared web test fixture: a ControlCenter with every service wired like the application, a
// recording command sink instead of Kafka, and the HTTP server on an ephemeral port.
#pragma once

#include <doctest.h>

#include <httplib.h>

#include <chrono>
#include <thread>

#include "algo/smart_buying/engine.h"
#include "algo/smart_selling/engine.h"
#include "core/config.h"
#include "core/i18n.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "test_helpers.h"
#include "web/control.h"
#include "web/server.h"
#include "web/sessions.h"

namespace ngtest {

using ng::json;

struct WebFixture {
    TempDir dir{"web"};
    ng::AppConfig config;
    std::unique_ptr<ng::web::ControlCenter> control;
    ng::web::SessionStore sessions;
    std::unique_ptr<ng::web::Server> server;
    ng::web::RecordingCommandSink* sink = nullptr;

    // `accounts_json` and `extra_config` let a test replace the default two accounts (A: Tiger API,
    // B: GigaMoney EMULATOR) or add configuration sections (leading comma included).
    explicit WebFixture(const std::string& accounts_json = "", const std::string& extra_config = "") {
        write_file(dir.file("accounts.json"),
                   accounts_json.empty() ? std::string(R"({"accounts": [
          {"string_id": "A", "numeric_id": 1, "broker": "Tiger", "trading_medium": "API", "monitor": true, "ip_address": "1.1.1.1"},
          {"string_id": "B", "numeric_id": 2, "broker": "GigaMoney", "trading_medium": "EMULATOR"}]})")
                                         : accounts_json);
        write_file(dir.file("config.json"), std::string(R"({
          "accounts_file": "./accounts.json", "symbols": ["AAPL", "MSFT"],
          "auth": {"users": {"admin": {"password": "admin-pw"}, "trader": {"password": "trader-pw"}, "lmh": {"password": "lmh-pw"}}},
          "server": {"host": "127.0.0.1", "port": 0, "ssl_enabled": false},
          "kafka": {"bootstrap_servers": "127.0.0.1:1", "market_data_bootstrap_servers": "127.0.0.1:1"},
          "smart_selling": {"state_db": "./smart.sqlite3"},
          "smart_buying": {"state_db": "./buying.sqlite3"},
          "reporting": {"db_path": "./reports/activity.sqlite3"})") + extra_config + "}");
        unsetenv("UI_SSL_ENABLED");
        unsetenv("REPORTING_DB_PATH");
        config = ng::load_config(dir.file("config.json"));
        config.operations_log_dir = dir.file("logs");
        control = std::make_unique<ng::web::ControlCenter>(config, ng::load_account_metas(config), ng::load_symbols(config));
        auto recording = std::make_unique<ng::web::RecordingCommandSink>();
        sink = recording.get();
        control->set_command_sink(std::move(recording));
        control->set_operations_log(
            std::make_unique<ng::OperationsLog>(dir.file("logs"), ng::wall_now(), [this] { return control->served_snapshots(); }));
        control->create_reporting(config.reporting.db_path);
        control->create_fast_trading_manager();
        control->create_smart_selling_runtime(std::make_shared<ng::smart::SmartSellingEngine>(":memory:"));
        control->create_smart_buying_runtime(std::make_shared<ng::smart::SmartBuyingEngine>(":memory:"));
        control->smart_runtime()->set_feed_states_for_test({{"orders", "live"}, {"volume", "live"}});
        control->buying_runtime()->set_feed_states_for_test({{"orders", "live"}, {"volume", "live"}});
        control->start_background();
        ng::web::ServerOptions options;
        options.host = "127.0.0.1";
        options.port = 0;
        options.threads = 8;
        server = std::make_unique<ng::web::Server>(*control, sessions, options);
        server->start();
        // The exact-name entitlement checks are exercised with look-alike user names too.
        for (const char* user : {"trader", "lmh", "admin", "LMH", " lmh "}) sessions.insert_for_test(token_for(user), user);
    }
    ~WebFixture() {
        server->stop();
        control->shutdown();
    }

    static std::string token_for(const std::string& user) {
        if (user == "LMH") return "upper-lmh-token";
        if (user == " lmh ") return "padded-lmh-token";
        return user + "-token";
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
    httplib::Client client_for(const std::string& user, const std::string& lang = "") { return client(token_for(user), lang); }

    void upsert_account(const std::string& id, double usd, double aapl_qty, bool trading = true, const std::string& symbol = "AAPL") {
        ng::AccountSnapshot s;
        s.account_id = id;
        s.cash = usd;
        s.cash_by_currency["USD"] = usd;
        s.available_cash_by_currency["USD"] = usd;
        if (aapl_qty) s.positions.push_back(ng::Position{symbol, aapl_qty, 90.0, aapl_qty});
        s.ts = ng::iso_utc_now();
        s.trading_enabled = trading;
        control->account_store().upsert(s);
    }
    void upsert_book(const std::string& symbol, double bid, double ask) {
        json payload = {{"symbol", symbol},
                        {"update_time", static_cast<long long>(ng::wall_now() * 1e9)},
                        {"bid_side", {{"levels", json::array({{{"prc", bid}, {"qty", 1000}}, {{"prc", bid - 1}, {"qty", 500}}})}}},
                        {"offer_side", {{"levels", json::array({{{"prc", ask}, {"qty", 800}}})}}}};
        control->market_store().upsert(*ng::quote_row_from_price_book(payload));
        control->market_store().upsert_book(*ng::order_book_from_price_book(payload));
    }
    // A complete live trade snapshot observed now.
    void upsert_quote(const std::string& symbol, double last, std::optional<double> prev_close = std::nullopt,
                      std::optional<long long> volume = std::nullopt, double age_seconds = 0) {
        ng::StockQuote quote;
        quote.symbol = symbol;
        quote.last = last;
        quote.observed_at = ng::wall_now() - age_seconds;
        quote.trade_time = quote.observed_at - 1;
        quote.is_delayed = false;
        quote.is_halted = false;
        quote.prev_close = prev_close;
        quote.volume = volume;
        control->market_store().upsert_stock_quote(quote);
    }
    json fast_start(const std::string& command_id, const std::string& mode, const std::string& symbol,
                    std::vector<std::string> accounts, bool test_mode = true) {
        return json{{"command_id", command_id}, {"trading_mode", mode}, {"symbol", symbol}, {"fast_trading_price_limit", 10},
                    {"fast_trading_account_ids", accounts}, {"fast_trading_book_levels", 1},
                    {"fast_trading_target_percentage", 100}, {"fast_trading_cycle_seconds", 5},
                    {"fast_trading_test_mode", test_mode}};
    }
    bool wait_until(const std::function<bool()>& done, int millis = 5000) {
        for (int i = 0; i < millis / 5 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return done();
    }
};

inline json body(const httplib::Result& res) {
    REQUIRE(res);
    return json::parse(res->body, nullptr, false);
}

}  // namespace ngtest
