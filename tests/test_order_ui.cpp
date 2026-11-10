// The order monitor (order_ui/tests/test_order_ui.py, test_reporting_api.py, test_reconciliation.py):
// snapshot validation, today's projection, shared configuration, the Kafka consumer and the HTTP API.
#include <doctest.h>

#include <httplib.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "core/sqlite.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "kafka_fixture.h"
#include "order_ui/config.h"
#include "order_ui/consumer.h"
#include "order_ui/reconciliation.h"
#include "order_ui/server.h"
#include "order_ui/store.h"
#include "orders/order_status.h"
#include "reporting/activity_reporting.h"
#include "test_helpers.h"

using ng::json;
using ng::order_ui::OrderServer;
using ng::order_ui::OrderStore;
using ng::order_ui::OrderUiConfig;
using ng::order_ui::ReconciliationSource;
using ng::order_ui::ReconciliationView;
using ng::reporting::ActivityReporting;
using Updates = std::initializer_list<std::pair<const char*, json>>;

namespace {

double epoch(const char* iso) { return ng::parse_iso8601(iso)->epoch_seconds; }

json payload(Updates updates = {}) {
    json out = {{"schema_version", 1}, {"account_id", "A"}, {"order_id", "order-1"}, {"sequence", 0},
                {"created_at", "2026-09-27T09:30:00-04:00"}, {"updated_at", "2026-09-27T09:30:00-04:00"}, {"symbol", "AAPL"},
                {"side", "BUY"}, {"order_type", "LIMIT"}, {"status", "NEW"}, {"quantity", 10}, {"filled_quantity", 0},
                {"average_fill_price", nullptr}, {"limit_price", 200}};
    for (const auto& [key, value] : updates) out[key] = value;
    return out;
}

ng::orders::OrderEvent event(Updates updates = {}) { return ng::orders::parse_order_message(payload(updates).dump()); }

struct StoreFixture {
    double now = epoch("2026-09-27T15:00:00+00:00");
    OrderStore store{ngtest::directory({ngtest::meta("A", 1, "Tiger"), ngtest::meta("B", 2, "Other")}), "America/New_York",
                     [this] { return now; }};
};

}  // namespace

TEST_CASE("order snapshots violating the schema or its cross-field rules are rejected") {
    for (const json& updates :
         {json{{"schema_version", 2}}, json{{"schema_version", true}}, json{{"sequence", -1}}, json{{"sequence", true}},
          json{{"sequence", 9007199254740992.0}}, json{{"status", "made_up"}}, json{{"account_id", " A"}}, json{{"account_id", ""}},
          json{{"quantity", 0}}, json{{"quantity", nullptr}}, json{{"filled_quantity", 11}}, json{{"filled_quantity", -1}},
          json{{"average_fill_price", -1}}, json{{"extra", "unknown field"}}, json{{"created_at", "2026-09-27T09:30:00"}},
          json{{"created_at", "2026-02-30T09:30:00Z"}}, json{{"updated_at", "2026-09-27T09:29:59-04:00"}}, json{{"status", "FILLED"}},
          json{{"status", "PARTIALLY_FILLED"}, {"filled_quantity", 10}}, json{{"status", "PARTIALLY_FILLED"}, {"filled_quantity", 0}},
          json{{"limit_price", nullptr}}, json{{"order_type", "STOP"}, {"stop_price", nullptr}}}) {
        CAPTURE(updates.dump());
        json snapshot = payload();
        snapshot.update(updates);
        CHECK_THROWS_AS(ng::orders::parse_order_message(snapshot.dump()), std::invalid_argument);
    }
    for (const std::string& raw : {std::string("no json"), std::string("[]"), std::string("null"), std::string("\xff"),
                                   std::string("{}"), std::string(65537, ' '), std::string(R"({"quantity": NaN})")}) {
        CAPTURE(raw.substr(0, 20));
        CHECK_THROWS_AS(ng::orders::parse_order_message(raw), std::invalid_argument);
    }
}

TEST_CASE("notional orders and fractional fills are supported") {
    auto snap = event({{"quantity", nullptr}, {"notional", 100}, {"order_type", "MARKET"}, {"status", "FILLED"},
                       {"filled_quantity", 0.45}, {"average_fill_price", 222.222222}, {"limit_price", nullptr}});
    CHECK(snap.payload["quantity"].is_null());
    CHECK(event({{"quantity", 0.5}, {"status", "PARTIALLY_FILLED"}, {"filled_quantity", 0.25}, {"average_fill_price", 200}})
              .payload["filled_quantity"] == 0.25);
}

TEST_CASE_FIXTURE(StoreFixture, "latest sequence wins and accounts have separate order ids") {
    CHECK(store.upsert(event({{"sequence", 2}, {"status", "FILLED"}, {"filled_quantity", 10}, {"average_fill_price", 200}})));
    CHECK_FALSE(store.upsert(event({{"sequence", 1}})));
    CHECK_FALSE(store.upsert(event({{"sequence", 2}})));
    CHECK(store.upsert(event({{"account_id", "B"}})));
    json rows = store.snapshot()["orders"];
    REQUIRE(rows.size() == 2);
    for (const auto& row : rows) {
        if (row["account_id"] == "A") CHECK(row["status"] == "FILLED");
    }
    CHECK(rows[0]["broker"].is_string());
    CHECK(rows[0]["account_num_id"].is_number_integer());
}

TEST_CASE_FIXTURE(StoreFixture, "immutable creation time and monotonic update time") {
    store.upsert(event({{"updated_at", "2026-09-27T10:00:00-04:00"}}));
    CHECK_THROWS_WITH_AS(store.upsert(event({{"sequence", 1}, {"created_at", "2026-09-27T09:29:00-04:00"}})),
                         "created_at is immutable for an order.", std::invalid_argument);
    CHECK_THROWS_WITH_AS(store.upsert(event({{"sequence", 1}})), "updated_at cannot decrease when sequence increases.",
                         std::invalid_argument);
    CHECK(store.snapshot()["orders"][0]["sequence"] == 0);
}

TEST_CASE_FIXTURE(StoreFixture, "only configured accounts with consistent ids") {
    CHECK_FALSE(store.upsert(event({{"account_id", "HIDDEN"}})));
    CHECK_FALSE(store.upsert(event({{"account_id", "UNKNOWN"}})));
    CHECK_THROWS_WITH_AS(store.upsert(event({{"account_num_id", 2}})), "account_num_id does not match the configured account_id.",
                         std::invalid_argument);
    CHECK(store.upsert(event({{"account_num_id", 1}})));
    CHECK(store.snapshot()["feed"]["ignored_messages"] == 2);
}

TEST_CASE_FIXTURE(StoreFixture, "today uses the creation date, not the update date, and rolls at midnight") {
    CHECK_FALSE(store.upsert(event({{"created_at", "2026-09-27T03:59:59Z"}, {"updated_at", "2026-09-27T14:00:00Z"}})));
    CHECK(store.upsert(event({{"created_at", "2026-09-27T04:00:00Z"}})));
    CHECK_FALSE(store.upsert(event({{"order_id", "tomorrow"}, {"created_at", "2026-09-28T04:00:00Z"}, {"updated_at", "2026-09-28T04:00:00Z"}})));
    now = epoch("2026-09-28T03:59:59+00:00");
    CHECK(store.snapshot()["orders"].size() == 1);
    now = epoch("2026-09-28T04:00:00+00:00");
    CHECK(store.snapshot()["date"] == "2026-09-28");
    CHECK(store.snapshot()["orders"].empty());
    CHECK_FALSE(store.upsert(event({{"updated_at", "2026-09-28T05:00:00Z"}})));
}

TEST_CASE("New York daylight saving boundaries") {
    struct Case { const char* now; const char* start; };
    for (Case c : {Case{"2026-03-08T12:00:00Z", "2026-03-08T05:00:00Z"}, Case{"2026-11-01T12:00:00Z", "2026-11-01T04:00:00Z"}}) {
        StoreFixture f;
        f.now = epoch(c.now);
        CHECK(f.store.upsert(event({{"created_at", c.start}, {"updated_at", c.now}})));
        CHECK(f.store.snapshot()["orders"].size() == 1);
    }
}

namespace {

struct ConfigFixture {
    ngtest::TempDir dir{"order-ui-config"};
    json config;
    ConfigFixture() {
        unsetenv("TRADING_ACCOUNTS_CONFIG");
        unsetenv("ORDER_UI_CONFIG");
        unsetenv("ACCOUNT_DASHBOARD_CONFIG");
        unsetenv("REPORTING_DB_PATH");
        ngtest::write_file(dir.file("accounts.json"), R"({"accounts": [
            {"string_id": "A", "numeric_id": 1, "broker": "Tiger", "trading_medium": "API"},
            {"string_id": "HIDDEN", "numeric_id": 2, "broker": "Other", "trading_medium": "WEB", "hidden": true}]})");
        config = {{"accounts_file", "accounts.json"},
                  {"reporting", {{"db_path", dir.file("reports.sqlite3")}}},
                  {"auth", {{"users", {{"admin", {{"password", "test-admin"}}}, {"trader", {{"password", "test-trader"}}},
                                       {"extra", {{"password", "test-extra"}}}}}}},
                  {"kafka", {{"bootstrap_servers", "shared-broker:9092"}}},
                  {"server", {{"host", "other-host"}, {"port", 8000}, {"ssl_enabled", true}, {"ssl_certfile", "missing.crt"}}}};
        write();
    }
    void write() { ngtest::write_file(path(), config.dump()); }
    std::string path() const { return dir.file("config.json"); }
    OrderUiConfig load() { return ng::order_ui::load_config(path()); }
};

}  // namespace

TEST_CASE_FIXTURE(ConfigFixture, "shared config reuses accounts, auth and brokers but its own server") {
    OrderUiConfig cfg = load();
    CHECK(cfg.accounts.ids_by_num_id() == std::vector<std::string>{"A"});
    CHECK(cfg.users.size() == 3);
    CHECK(cfg.users.at("extra") == "test-extra");
    CHECK(cfg.server.port == 8030);
    CHECK(cfg.server.host == "127.0.0.1");
    CHECK(cfg.server.ssl_certfile.empty());
    CHECK(cfg.kafka.bootstrap_servers == "shared-broker:9092");
    CHECK(cfg.kafka.topic == "order-status");
    CHECK(cfg.kafka.group_id == "order-dashboard");
    CHECK(cfg.timezone == "America/New_York");
    CHECK(cfg.refresh_interval_seconds == 2);
    CHECK(cfg.reporting_db_path == dir.file("reports.sqlite3"));
    REQUIRE(cfg.reconciliation_sources.size() == 2);
    CHECK(cfg.reconciliation_sources[0].mode == "G");
    CHECK(cfg.reconciliation_sources[1].mode == "H");
}

TEST_CASE_FIXTURE(ConfigFixture, "config aliases, inline accounts and defaults") {
    config["trading_accounts_file"] = config["accounts_file"];
    config.erase("accounts_file");
    write();
    CHECK(load().accounts.ids_by_num_id() == std::vector<std::string>{"A"});
    config.erase("trading_accounts_file");
    setenv("TRADING_ACCOUNTS_CONFIG", "accounts.json", 1);
    write();
    CHECK(load().accounts.ids_by_num_id() == std::vector<std::string>{"A"});
    unsetenv("TRADING_ACCOUNTS_CONFIG");
    CHECK_THROWS_WITH_AS(load(), "Provide accounts_file or a non-empty accounts list, as in the trading UI.", ng::ConfigError);
    config["accounts"] = json::array({{{"id", "INLINE"}, {"num_id", 4}, {"broker", "Other"}}});
    config["order_ui"] = {{"timezone", "UTC"}, {"server", {{"port", 9000}}}, {"kafka", {{"topic", "custom-orders"}}}};
    write();
    OrderUiConfig loaded = load();
    CHECK(loaded.accounts.ids_by_num_id() == std::vector<std::string>{"INLINE"});
    CHECK(loaded.kafka.topic == "custom-orders");
    CHECK(loaded.server.port == 9000);
    CHECK(loaded.timezone == "UTC");
    setenv("ACCOUNT_DASHBOARD_CONFIG", path().c_str(), 1);
    CHECK(ng::order_ui::default_config_path(dir.path.string()) == path());
    setenv("ORDER_UI_CONFIG", "/explicit/order.json", 1);
    CHECK(ng::order_ui::default_config_path(dir.path.string()) == "/explicit/order.json");
    unsetenv("ACCOUNT_DASHBOARD_CONFIG");
    unsetenv("ORDER_UI_CONFIG");
    CHECK(ng::order_ui::default_config_path(dir.path.string()) == path());
    ngtest::write_file(dir.file("config.local.json"), "{}");
    CHECK(ng::order_ui::default_config_path(dir.path.string()) == dir.file("config.local.json"));
}

TEST_CASE_FIXTURE(ConfigFixture, "invalid order UI config fails with a clear error") {
    for (const json& settings :
         {json{{"timezone", "Invalid/Zone"}}, json{{"refresh_interval_seconds", 0}}, json{{"refresh_interval_seconds", true}},
          json{{"server", {{"port", 0}}}}, json{{"server", {{"port", 8000.5}}}}, json{{"server", {{"ssl_certfile", "missing"}}}},
          json{{"kafka", {{"poll_timeout_sec", 99}}}}, json{{"kafka", {{"topic", ""}}}},
          json{{"kafka", {{"client_config", {{"group.id", "trading-ui"}}}}}}, json{{"unknown", 1}},
          json{{"server", {{"extra", 1}}}}, json{{"kafka", {{"extra", 1}}}}, json{{"kafka", {{"client_config", {{"sasl.username", 5}}}}}}}) {
        CAPTURE(settings.dump());
        config["order_ui"] = settings;
        write();
        CHECK_THROWS_AS(load(), ng::ConfigError);
    }
    config["order_ui"] = json{{"unknown", 1}, {"another", 2}};
    write();
    CHECK_THROWS_WITH_AS(load(), "Unknown order_ui setting(s): another, unknown", ng::ConfigError);
    config["order_ui"] = json{{"kafka", {{"client_config", {{"ssl.ca.location", "certs/ca.pem"}, {"security.protocol", "SSL"}}}}}};
    write();
    OrderUiConfig cfg = load();
    CHECK(cfg.kafka.client_config["ssl.ca.location"] == dir.file("certs/ca.pem"));
    CHECK(cfg.kafka.client_config["security.protocol"] == "SSL");
    config.erase("order_ui");
    config["auth"]["users"].erase("trader");
    write();
    CHECK_THROWS_WITH_AS(load(), "auth.users.trader.password is required, as in the trading UI.", ng::ConfigError);
}

TEST_CASE_FIXTURE(ConfigFixture, "shared config resolves explicit Smart controller state paths") {
    config["smart_selling"] = {{"state_db", "custom/sell.sqlite3"}};
    config["smart_buying"] = {{"state_db", dir.file("buy.sqlite3")}};
    write();
    auto sources = load().reconciliation_sources;
    CHECK(sources[0].state_db == dir.file("custom/sell.sqlite3"));
    CHECK(sources[1].state_db == dir.file("buy.sqlite3"));
}

TEST_CASE("order consumer replays from the beginning, skips bad data and uses independent groups") {
    ngtest::MockCluster cluster;
    cluster.topic("order-status", 2);
    StoreFixture f;
    ng::order_ui::OrderKafkaConfig kafka;
    kafka.bootstrap_servers = cluster.bootstraps;
    kafka.poll_timeout_sec = 0.2;
    // Retained history: a valid order (two sequences on different partitions do not matter), malformed
    // data, a tombstone-like empty JSON and a snapshot whose numeric id contradicts the configuration.
    ngtest::produce(cluster.bootstraps, "order-status", "k1", payload().dump());
    ngtest::produce(cluster.bootstraps, "order-status", "k2", "bad");
    ngtest::produce(cluster.bootstraps, "order-status", "k3", "[]");
    ngtest::produce(cluster.bootstraps, "order-status", "k4", payload({{"account_num_id", 99}}).dump());
    ngtest::produce(cluster.bootstraps, "order-status", "k5",
                    payload({{"order_id", "order-2"}, {"account_id", "B"}, {"sequence", 3}}).dump());
    ng::order_ui::OrderStatusConsumer first(kafka, f.store), second(kafka, f.store);
    CHECK(first.group_id() != second.group_id());
    CHECK(ng::starts_with(first.group_id(), "order-dashboard-"));
    CHECK(f.store.snapshot()["feed"]["state"] == "starting");
    first.start();
    REQUIRE(ngtest::kafka_wait_until([&] { return f.store.snapshot()["feed"]["state"] == "live"; }));
    json snapshot = f.store.snapshot();
    CHECK(snapshot["feed"]["detail"] == "Kafka connected");
    CHECK(snapshot["feed"]["invalid_messages"] == 3);
    CHECK(snapshot["orders"].size() == 2);
    CHECK(snapshot["feed"]["last_message_at"].is_string());
    // New data keeps flowing after the replay.
    ngtest::produce(cluster.bootstraps, "order-status", "k1",
                    payload({{"sequence", 1}, {"status", "FILLED"}, {"filled_quantity", 10}, {"average_fill_price", 200}}).dump());
    REQUIRE(ngtest::kafka_wait_until([&] {
        json current = f.store.snapshot();
        for (const auto& row : current["orders"]) {
            if (row["order_id"] == "order-1" && row["status"] == "FILLED") return true;
        }
        return false;
    }));
    first.stop();
    CHECK(f.store.snapshot()["feed"]["state"] == "stopped");
    CHECK(f.store.snapshot()["feed"]["detail"] == "Order consumer stopped");
    // A second, independent dashboard replays the whole retained topic for itself.
    StoreFixture other;
    ng::order_ui::OrderStatusConsumer independent(kafka, other.store);
    independent.start();
    REQUIRE(ngtest::kafka_wait_until([&] { return other.store.snapshot()["feed"]["state"] == "live"; }));
    CHECK(other.store.snapshot()["orders"].size() == 2);
    independent.stop();
}

namespace {

const char* kRequestHeader = "X-Order-UI-Request";

// The services of one order-server process on temporary databases, with controllable clocks.
struct OrderUiFixture {
    ngtest::TempDir dir{"order-ui"};
    double now = epoch("2026-10-28T14:00:00+00:00");
    double session_shift = 0;  // added to the wall clock used for session expiry
    OrderUiConfig config;
    std::unique_ptr<ActivityReporting> reporting;
    std::unique_ptr<ReconciliationView> reconciliation;
    std::unique_ptr<OrderStore> store;
    std::unique_ptr<OrderServer> server;
    std::string cookie;
    std::string extra_cookie;

    explicit OrderUiFixture(const std::string& timezone = "America/New_York") {
        config.accounts = ngtest::directory({ngtest::meta("A", 1, "Tiger"), ngtest::meta("B", 2, "Futu")});
        config.users = {{"trader", "test-password"}, {"extra", "test-extra"}};
        config.timezone = timezone;
        config.refresh_interval_seconds = 2;
        config.reporting_db_path = dir.file("activity.sqlite3");
        open();
    }
    ~OrderUiFixture() { close(); }

    // Starts (or restarts) the server process on the same report database.
    void open() {
        close();
        reporting = std::make_unique<ActivityReporting>(config.reporting_db_path, [this] { return now; });
        reconciliation = std::make_unique<ReconciliationView>(config.reconciliation_sources, config.reporting_db_path);
        store = std::make_unique<OrderStore>(config.accounts, config.timezone, [this] { return now; },
                                             [this](const ng::orders::OrderEvent& e) { reporting->record_order(e); });
        store->update_feed(std::string("live"), std::string("Live"));
        ng::order_ui::OrderServerOptions options;
        options.port = 0;
        server = std::make_unique<OrderServer>(config, *store, *reporting, *reconciliation, options,
                                               [this] { return ng::wall_now() + session_shift; });
        server->start();
        cookie.clear();
    }
    void close() {
        if (server) server->stop();
        server.reset();
        store.reset();
        reconciliation.reset();
        if (reporting) reporting->close();
        reporting.reset();
    }

    httplib::Client client() {
        httplib::Client c("127.0.0.1", server->port());
        c.set_connection_timeout(5, 0);
        c.set_read_timeout(5, 0);
        return c;
    }
    httplib::Headers headers(bool request_header, const std::string& origin = "") {
        httplib::Headers out;
        std::string cookies = extra_cookie;
        if (!cookie.empty()) cookies += (cookies.empty() ? "" : "; ") + std::string(ng::order_ui::kSessionCookie) + "=" + cookie;
        if (!cookies.empty()) out.emplace("Cookie", cookies);
        if (request_header) out.emplace(kRequestHeader, "1");
        if (!origin.empty()) out.emplace("Origin", origin);
        return out;
    }
    httplib::Result get(const std::string& path) { return client().Get(path, headers(false)); }
    httplib::Result post(const std::string& path, const json& body = json::object(), bool request_header = true,
                         const std::string& origin = "") {
        return client().Post(path, headers(request_header, origin), body.dump(), "application/json");
    }
    httplib::Result login(const std::string& username = "trader", const std::string& password = "test-password") {
        auto response = post("/api/login", json{{"username", username}, {"password", password}});
        if (response && response->status == 200) {
            std::string header = response->get_header_value("Set-Cookie");
            std::string prefix = std::string(ng::order_ui::kSessionCookie) + "=";
            REQUIRE(header.rfind(prefix, 0) == 0);
            cookie = header.substr(prefix.size(), header.find(';') - prefix.size());
        }
        return response;
    }
    json report() {
        auto response = get("/api/orders");
        REQUIRE(response);
        REQUIRE(response->status == 200);
        return json::parse(response->body);
    }
    std::string start_tag(const std::string& name = "Opening", const std::string& symbol = "AAPL") {
        auto response = post("/api/tags", json{{"name", name}, {"symbol", symbol}});
        REQUIRE(response);
        REQUIRE(response->status == 201);
        return json::parse(response->body)["id"].get<std::string>();
    }
};

ng::orders::OrderEvent report_event(Updates updates = {}) {
    json out = {{"schema_version", 1}, {"account_id", "A"}, {"order_id", "order-1"}, {"sequence", 0},
                {"created_at", "2026-10-28T14:00:00Z"}, {"updated_at", "2026-10-28T14:00:00Z"}, {"symbol", "AAPL"},
                {"side", "BUY"}, {"order_type", "MARKET"}, {"status", "PARTIALLY_FILLED"}, {"quantity", 10},
                {"filled_quantity", 2}, {"average_fill_price", 100}, {"currency", "USD"}};
    for (const auto& [key, value] : updates) out[key] = value;
    return ng::orders::parse_order_message(out.dump());
}

}  // namespace

TEST_CASE_FIXTURE(OrderUiFixture, "login, authentication, cookie isolation, logout and no secrets") {
    CHECK(get("/api/orders")->status == 401);
    auto html = get("/");
    REQUIRE(html);
    CHECK(html->status == 200);
    CHECK(html->body.find("test-password") == std::string::npos);
    CHECK(html->get_header_value("Cache-Control") == "no-store");
    CHECK(html->get_header_value("X-Content-Type-Options") == "nosniff");
    CHECK(html->get_header_value("X-Frame-Options") == "DENY");
    CHECK(html->get_header_value("Content-Security-Policy").find("default-src 'self'") == 0);
    CHECK(html->get_header_value("Content-Type").find("text/html") == 0);
    auto script = get("/static/app.js");
    CHECK(script->status == 200);
    CHECK(script->get_header_value("Cache-Control") == "no-store");
    CHECK(get("/static/../CMakeLists.txt")->status == 404);
    CHECK(get("/static/missing.js")->status == 404);
    // The trading UI's own session cookie is neither read nor replaced.
    extra_cookie = "auth_token=existing-trading-ui-session";
    auto response = login();
    REQUIRE(response);
    CHECK(response->status == 200);
    std::string set_cookie = response->get_header_value("Set-Cookie");
    CHECK(set_cookie.find(ng::order_ui::kSessionCookie) == 0);
    CHECK(set_cookie.find("HttpOnly") != std::string::npos);
    CHECK(set_cookie.find("SameSite=strict") != std::string::npos);
    CHECK(set_cookie.find("Max-Age=43200") != std::string::npos);
    CHECK(set_cookie.find("auth_token") == std::string::npos);
    store->upsert(report_event());
    auto data = get("/api/orders");
    json body = json::parse(data->body);
    CHECK(body["orders"][0]["order_id"] == "order-1");
    CHECK(body["accounts"] == json::array({{{"id", "A"}, {"num_id", 1}, {"broker", "Tiger"}}, {{"id", "B"}, {"num_id", 2}, {"broker", "Futu"}}}));
    CHECK(body["username"] == "trader");
    CHECK(body["refresh_interval_seconds"] == 2.0);
    CHECK(body["statuses"] == json(ng::orders::order_statuses()));
    CHECK(body["timezone"] == "America/New_York");
    CHECK(body["feed"]["state"] == "live");
    CHECK(data->body.find("password") == std::string::npos);
    CHECK(post("/api/logout")->status == 200);
    CHECK(get("/api/orders")->status == 401);
}

TEST_CASE_FIXTURE(OrderUiFixture, "the order page, its script, translations and shared fonts are served") {
    auto html = get("/");
    REQUIRE(html);
    REQUIRE(html->status == 200);
    auto script = get("/static/app.js");
    auto i18n = get("/static/i18n.js");
    auto styles = get("/static/styles.css");
    REQUIRE(script->status == 200);
    REQUIRE(i18n->status == 200);
    REQUIRE(styles->status == 200);
    CHECK(styles->get_header_value("Content-Type").find("text/css") == 0);
    CHECK(script->body.find("/api/orders") != std::string::npos);  // the order UI's own script
    auto font = get("/static/fonts/ktrader-cjk.woff2");
    CHECK(font->status == 200);
    CHECK(font->get_header_value("Content-Type") == "font/woff2");
    CHECK(get("/static/favicon.svg")->status == 200);
    // The script, style sheet and markup must not need inline code: the CSP forbids it.
    CHECK(html->body.find("<script>") == std::string::npos);
    CHECK(html->body.find(" style=") == std::string::npos);
    for (const char* handler : {"onclick=", "onload=", "onsubmit=", "oninput=", "onchange="}) {
        CHECK(html->body.find(handler) == std::string::npos);
    }
    // Every element the script looks up exists in the page, and every translated label has a string.
    std::regex lookup(R"re(el\("([A-Za-z0-9_-]+)"\))re");
    std::set<std::string> ids;
    for (std::sregex_iterator it(script->body.begin(), script->body.end(), lookup), end; it != end; ++it) ids.insert((*it)[1]);
    CHECK(ids.size() > 50);
    for (const std::string& id : ids) {
        CAPTURE(id);
        CHECK(html->body.find("id=\"" + id + "\"") != std::string::npos);
    }
    std::regex label(R"re(data-i18n(?:-label|-placeholder)?="([a-z0-9_]+)")re");
    std::set<std::string> keys;
    for (std::sregex_iterator it(html->body.begin(), html->body.end(), label), end; it != end; ++it) keys.insert((*it)[1]);
    CHECK(keys.size() > 50);
    for (const std::string& key : keys) {
        CAPTURE(key);
        bool plain = i18n->body.find(" " + key + ":") != std::string::npos || i18n->body.find("{" + key + ":") != std::string::npos;
        CHECK((plain || i18n->body.find("\"" + key + "\":") != std::string::npos));
    }
}

TEST_CASE_FIXTURE(OrderUiFixture, "wrong credentials, cross-origin requests and expired sessions") {
    json body = {{"username", "trader"}, {"password", "wrong"}};
    auto wrong = post("/api/login", body);
    CHECK(wrong->status == 401);
    CHECK(json::parse(wrong->body)["detail"] == "Incorrect username or password.");
    CHECK(post("/api/login", body, false)->status == 403);
    CHECK(post("/api/login", body, true, "https://other.example")->status == 403);
    CHECK(post("/api/login", json{{"username", "nobody"}, {"password", "test-password"}})->status == 401);
    CHECK(post("/api/login", json{{"username", ""}, {"password", "x"}})->status == 422);
    CHECK(post("/api/login", json{{"username", "trader"}})->status == 422);
    CHECK(client().Post("/api/login", headers(true), "not json", "application/json")->status == 422);
    // The browser's own origin is accepted; names are trimmed like the Python service.
    std::string origin = "http://127.0.0.1:" + std::to_string(server->port());
    CHECK(client().Post("/api/login", httplib::Headers{{kRequestHeader, "1"}, {"Origin", origin}, {"Host", "127.0.0.1:" + std::to_string(server->port())}},
                        json{{"username", " extra "}, {"password", "test-extra"}}.dump(), "application/json")->status == 200);
    CHECK(login("extra", "test-extra")->status == 200);
    CHECK(report()["username"] == "extra");
    session_shift = ng::order_ui::kSessionSeconds + 1;
    CHECK(get("/api/orders")->status == 401);
    session_shift = 0;
    CHECK(get("/api/orders")->status == 401);  // an expired session is gone for good
}

TEST_CASE("order API updates without navigation and handles midnight") {
    OrderUiFixture f;
    f.now = epoch("2026-09-27T15:00:00+00:00");
    f.login();
    CHECK(f.report()["orders"].empty());
    f.store->upsert(event());
    CHECK(f.report()["orders"][0]["status"] == "NEW");
    f.store->upsert(event({{"sequence", 1}, {"status", "FILLED"}, {"filled_quantity", 10}, {"average_fill_price", 200}}));
    CHECK(f.report()["orders"][0]["status"] == "FILLED");
    f.now = epoch("2026-09-28T04:00:00Z");
    CHECK(f.report()["orders"].empty());
}

TEST_CASE_FIXTURE(OrderUiFixture, "tag routes require authentication and the same-origin request header") {
    json body = {{"name", "Opening"}, {"symbol", "AAPL"}};
    CHECK(get("/api/orders")->status == 401);
    CHECK(post("/api/tags", body)->status == 401);
    CHECK(post("/api/tags/missing/stop")->status == 401);
    login();
    CHECK(post("/api/tags", body, false)->status == 403);
    CHECK(post("/api/tags/missing/stop", json::object(), false)->status == 403);
    CHECK(post("/api/tags", body, true, "https://unrelated.example")->status == 403);
    CHECK(post("/api/tags/missing/stop", json::object(), true, "https://unrelated.example")->status == 403);
    CHECK(report()["reporting"]["tags"].empty());
}

TEST_CASE_FIXTURE(OrderUiFixture, "invalid tag input is rejected without creating a tag") {
    login();
    for (const json& body : {json{{"name", ""}, {"symbol", "AAPL"}}, json{{"name", "   "}, {"symbol", "AAPL"}},
                             json{{"name", "Bad\nname"}, {"symbol", "AAPL"}}, json{{"name", std::string("Bad\0name", 8)}, {"symbol", "AAPL"}},
                             json{{"name", "Opening"}, {"symbol", ""}}, json{{"name", "Opening"}, {"symbol", "   "}},
                             json{{"name", "Opening"}, {"symbol", "AA PL"}}, json{{"name", "Opening"}, {"symbol", std::string("AA\0PL", 5)}},
                             json{{"name", std::string(101, 'x')}, {"symbol", "AAPL"}}, json{{"name", "Opening"}, {"symbol", std::string(65, 'X')}},
                             json{{"name", 5}, {"symbol", "AAPL"}}, json{{"symbol", "AAPL"}}}) {
        CAPTURE(body.dump());
        CHECK(post("/api/tags", body)->status == 422);
        CHECK(report()["reporting"]["tags"].empty());
    }
}

TEST_CASE_FIXTURE(OrderUiFixture, "tag conflict, stop idempotence, missing tag and order labels") {
    login();
    std::string tag_id = start_tag("  Opening buys  ", "aapl");
    CHECK(post("/api/tags", json{{"name", "Other"}, {"symbol", "MSFT"}})->status == 409);
    store->upsert(report_event());
    json data = report();
    CHECK(data["orders"][0]["tags"] == json::array({{{"id", tag_id}, {"name", "Opening buys"}}}));
    CHECK(data["reporting"]["active_tag_id"] == tag_id);
    now += 60;
    CHECK(post("/api/tags/" + tag_id + "/stop")->status == 200);
    json stopped_at = report()["reporting"]["tags"][0]["stopped_at"];
    CHECK(stopped_at == "2026-10-28T14:01:00Z");
    now += 60;
    CHECK(post("/api/tags/" + tag_id + "/stop")->status == 200);
    CHECK(report()["reporting"]["tags"][0]["stopped_at"] == stopped_at);
    CHECK(report()["reporting"]["active_tag_id"].is_null());
    CHECK(post("/api/tags/missing/stop")->status == 404);
    std::string new_id = start_tag("Later buys");
    CHECK(new_id != tag_id);
    CHECK(report()["orders"][0]["tags"] == json::array({{{"id", tag_id}, {"name", "Opening buys"}}}));
}

TEST_CASE_FIXTURE(OrderUiFixture, "the active tag and trade totals survive an order server restart") {
    login();
    std::string tag_id = start_tag();
    store->upsert(report_event());
    now += 60;
    open();
    CHECK(get("/api/orders")->status == 401);  // sessions are in memory only
    login();
    json data = report();
    CHECK(data["reporting"]["active_tag_id"] == tag_id);
    CHECK(data["reporting"]["tags"][0]["totals"]["shares_bought"] == 2.0);
    // The broker replays its retained latest snapshot after reconnecting.
    store->upsert(report_event());
    data = report();
    CHECK(data["orders"][0]["tags"] == json::array({{{"id", tag_id}, {"name", "Opening"}}}));
    CHECK(data["reporting"]["tags"][0]["totals"]["shares_bought"] == 2.0);
}

TEST_CASE_FIXTURE(OrderUiFixture, "the store keeps tagged fills across midnight after recent orders expire") {
    login();
    now = epoch("2026-10-29T03:59:00+00:00");
    std::string tag_id = start_tag();
    CHECK(store->upsert(report_event({{"created_at", "2026-10-29T03:59:00Z"}, {"updated_at", "2026-10-29T03:59:00Z"}})));
    CHECK(report()["orders"].size() == 1);
    now += 120;
    CHECK(report()["orders"].empty());
    CHECK_FALSE(store->upsert(report_event({{"sequence", 1}, {"filled_quantity", 6}, {"created_at", "2026-10-29T03:59:00Z"},
                                            {"updated_at", "2026-10-29T04:01:00Z"}})));
    json data = report();
    CHECK(data["date"] == "2026-10-29");
    CHECK(data["orders"].empty());
    CHECK(data["reporting"]["active_tag_id"] == tag_id);
    CHECK(data["reporting"]["tags"][0]["totals"]["cash_by_currency"]["USD"]["buy_cost"] == 600.0);
}

TEST_CASE_FIXTURE(OrderUiFixture, "the store excludes future, hidden and mismatched accounts from reports") {
    login();
    start_tag();
    CHECK_FALSE(store->upsert(report_event({{"account_id", "HIDDEN"}})));
    CHECK_FALSE(store->upsert(report_event({{"created_at", "2026-10-29T14:00:00Z"}, {"updated_at", "2026-10-29T14:00:00Z"}})));
    CHECK_THROWS_WITH_AS(store->upsert(report_event({{"account_num_id", 2}})), "account_num_id does not match the configured account_id.",
                         std::invalid_argument);
    json data = report();
    CHECK(data["orders"].empty());
    CHECK(data["reporting"]["tags"][0]["accounts"].empty());
    CHECK(data["reporting"]["tags"][0]["totals"]["shares_bought"] == 0.0);
}

TEST_CASE_FIXTURE(OrderUiFixture, "a session report includes every participating account and confirmed cash") {
    login();
    json parameters = {{"symbol", "AAPL"}, {"trading_mode", "F"}, {"fast_trading_price_limit", 100}};
    reporting->start_session("session", parameters, {"A", "B"});
    reporting->register_command("session", json{{"account_id", "A"}, {"command_id", "child-A"}});
    reporting->register_command("session", json{{"account_id", "B"}, {"command_id", "child-B"}});
    store->upsert(report_event({{"command_id", "child-A"}}));
    json first = report()["reporting"]["sessions"][0];
    CHECK(first["parameters"] == parameters);
    CHECK(first["running"] == true);
    REQUIRE(first["accounts"].size() == 2);
    CHECK(first["accounts"][0]["account_id"] == "A");
    CHECK(first["accounts"][1]["account_id"] == "B");
    CHECK(first["accounts"][1]["shares_sold"] == 0.0);
    store->upsert(report_event({{"account_id", "B"}, {"command_id", "child-B"}, {"side", "SELL"}, {"filled_quantity", 3},
                                {"average_fill_price", 110}}));
    json second = report()["reporting"]["sessions"][0];
    CHECK(second["totals"]["shares_bought"] == 2.0);
    CHECK(second["totals"]["shares_sold"] == 3.0);
    CHECK(second["totals"]["cash_by_currency"]["USD"]["cash_change"] == 130.0);
}

TEST_CASE("session reports hide participation without changing the saved parameters") {
    for (const char* trading_mode : {"G", "H"}) {
        CAPTURE(std::string(trading_mode));
        OrderUiFixture f;
        json visible = {{"symbol", "AAPL"}, {"trading_mode", trading_mode}, {"mode", "normal"}, {"quantity", "1000"}};
        json parameters = visible;
        parameters["participation"] = "0.12";
        parameters["participation_percentage"] = "12";
        parameters["participation_percentages"] = {{"patient", "6"}, {"normal", "12"}, {"urgent", "20"}, {"ultra", "30"}};
        parameters["settings"] = json::array({{{"participation_percentage", "12"}, {"mode", "normal"}}});
        json expected = visible;
        expected["settings"] = json::array({{{"mode", "normal"}}});
        f.login();
        f.reporting->start_session("private-session", parameters, {"A", "B"});
        json session = f.report()["reporting"]["sessions"][0];
        CHECK(session["running"] == true);
        CHECK(session["parameters"] == expected);
        CHECK(f.reporting->snapshot(f.config.accounts)["sessions"][0]["parameters"] == parameters);
        f.reporting->end_session("private-session");
        // Existing history is filtered on read, including after a server restart.
        f.open();
        f.login();
        session = f.report()["reporting"]["sessions"][0];
        CHECK(session["running"] == false);
        CHECK(session["parameters"] == expected);
        CHECK(f.reporting->snapshot(f.config.accounts)["sessions"][0]["parameters"] == parameters);
    }
    CHECK(ng::order_ui::public_algo_parameters(json(5)) == 5);
    CHECK(ng::order_ui::public_algo_parameters(json::array({json{{"participation", 1}, {"a", json{{"participation_percentages", 2}, {"b", 3}}}}})) ==
          json::array({json{{"a", json{{"b", 3}}}}}));
}

// --- unreconciled orders (order_ui/tests/test_reconciliation.py) ---------------------------------

namespace {

struct ReconciliationFixture {
    ngtest::TempDir dir{"order-ui-reconciliation"};
    std::vector<ReconciliationSource> sources;
    std::string report = dir.file("activity.sqlite3");
    ng::AccountDirectory accounts = ngtest::directory({ngtest::meta("A", 1, "GigaMoney")});

    ReconciliationFixture() {
        for (const char* mode : {"G", "H"}) {
            std::string path = dir.file(std::string(mode) + ".sqlite3");
            write(path, "CREATE TABLE smart_parents (id TEXT PRIMARY KEY, active INTEGER, data TEXT)");
            write(feedback(path), "CREATE TABLE feedback_orders (account_id TEXT, order_id TEXT, symbol TEXT, side TEXT, status TEXT, "
                                  "sequence INTEGER, created_at TEXT, updated_at TEXT)");
            write(feedback(path), "CREATE TABLE feedback_pending_commands (command_id TEXT, account_id TEXT, symbol TEXT, pending INTEGER)");
            sources.push_back({mode, path});
        }
        write(report, "CREATE TABLE activity_orders (account_id TEXT, order_id TEXT, payload TEXT)");
    }
    static std::string feedback(const std::string& state) { return state + ".feedback.sqlite3"; }
    static void write(const std::string& path, const std::string& sql, const std::vector<std::string>& args = {}) {
        ng::Sqlite db(path);
        ng::Statement statement(db, sql);
        for (std::size_t i = 0; i < args.size(); ++i) statement.bind(static_cast<int>(i + 1), args[i]);
        statement.exec();
    }
    void broker(const ReconciliationSource& source, const std::string& account = "A", const std::string& status = "PENDING_NEW") {
        write(feedback(source.state_db), "INSERT INTO feedback_orders VALUES (?,'order-1','AEHL','SELL',?,0,'2026-10-26T15:00:00Z','2026-10-26T15:00:00Z')",
              {account, status});
    }
    void pending(const ReconciliationSource& source, const std::string& account = "A", const std::string& symbol = "POM",
                 const std::string& command_id = "limit_2026-10-26T15:32:47.123Z_123") {
        write(feedback(source.state_db), "INSERT INTO feedback_pending_commands VALUES (?,?,?,1)", {command_id, account, symbol});
    }
    void child(const ReconciliationSource& source, const json& broker_id = nullptr) {
        json data = {{"symbol", "AEHL"}, {"state", "STOPPING"},
                     {"order", {{"account_id", "A"}, {"client_order_id", "smart-child"}, {"broker_order_id", broker_id},
                                {"state", "RECONCILE"}, {"dispatch_prepared", true}, {"quantity", "100"},
                                {"cumulative_filled", "0"}, {"limit_price", "17.4"}, {"send_wall", 1793912348}}}};
        write(source.state_db, "INSERT INTO smart_parents VALUES ('session-1',1,?)", {data.dump()});
    }
    static std::string bytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }
    json item(const json& result, const char* kind) {
        for (const auto& row : result["items"]) {
            if (row["kind"] == kind) return row;
        }
        FAIL("row not found");
        return nullptr;
    }
};

}  // namespace

TEST_CASE_FIXTURE(ReconciliationFixture, "persistent blockers on all dates deduplicate and resolve without mutation") {
    for (const auto& source : sources) {
        broker(source);
        broker(source, "HIDDEN");
        pending(source);
        pending(source, "HIDDEN");
    }
    child(sources[0]);
    std::string message = "Broker status: VarietiesNotReported <img src=x onerror=alert(1)>";
    write(report, "INSERT INTO activity_orders VALUES ('A','order-1',?)",
          {json{{"status", "PENDING_NEW"}, {"quantity", 39809}, {"filled_quantity", 0}, {"message", message}}.dump()});
    std::vector<std::string> paths;
    for (const auto& source : sources) paths.push_back(source.state_db);
    for (const auto& source : sources) paths.push_back(feedback(source.state_db));
    std::vector<std::string> before;
    for (const auto& path : paths) before.push_back(bytes(path));
    ReconciliationView view(sources, report);
    json result = view.snapshot(accounts);
    CHECK(result["complete"] == true);
    CHECK(result["details_available"] == true);
    REQUIRE(result["items"].size() == 3);
    for (const auto& row : result["items"]) CHECK(row["account_id"] == "A");
    json order = item(result, "broker_order");
    CHECK(order["modes"] == json::array({"G", "H"}));
    CHECK(order["quantity"] == 39809);
    CHECK(order["message"] == message);
    CHECK(order["age_seconds"].get<double>() > 86400);
    CHECK(order["reason"] == "awaiting_acceptance");
    CHECK(order["reported_status"] == "PENDING_NEW");
    CHECK(order["account_num_id"] == 1);
    CHECK(order["broker"] == "GigaMoney");
    json command = item(result, "pending_command");
    CHECK(command["modes"] == json::array({"G", "H"}));
    CHECK(command["updated_at"] == "2026-10-26T15:32:47.123Z");
    CHECK(command["status"] == "AWAITING_REPORT");
    CHECK(command["reason"] == "missing_feedback");
    CHECK_FALSE(command.contains("quantity"));
    json algo = item(result, "algo_order");
    CHECK(algo["order_id"].is_null());
    CHECK(algo["command_id"] == "smart-child");
    CHECK(algo["reason"] == "algo_reconciliation");
    CHECK(algo["side"] == "SELL");
    CHECK(algo["session_id"] == "session-1");
    CHECK(algo["session_state"] == "STOPPING");
    CHECK(algo["quantity"] == "100");
    CHECK(algo["limit_price"] == "17.4");
    CHECK(algo["updated_at"] == ng::iso_utc_offset(1793912348));
    for (std::size_t i = 0; i < paths.size(); ++i) CHECK(bytes(paths[i]) == before[i]);
    // Newest evidence first.
    CHECK(result["items"][0]["kind"] == "algo_order");

    // A terminal update in only one controller must not hide another's blocker.
    write(feedback(sources[0].state_db), "UPDATE feedback_orders SET status='CANCELED'");
    order = item(view.snapshot(accounts), "broker_order");
    CHECK(order["modes"] == json::array({"H"}));
    for (const auto& source : sources) {
        write(feedback(source.state_db), "UPDATE feedback_orders SET status='CANCELED'");
        write(feedback(source.state_db), "UPDATE feedback_pending_commands SET pending=0");
        write(source.state_db, "UPDATE smart_parents SET active=0");
    }
    CHECK(view.snapshot(accounts)["items"].empty());
}

TEST_CASE_FIXTURE(ReconciliationFixture, "a bound child shares the broker row and a terminal report does not clear the controller") {
    broker(sources[0]);
    child(sources[0], "order-1");
    write(report, "INSERT INTO activity_orders VALUES ('A','order-1',?)",
          {json{{"status", "FILLED"}, {"quantity", 100}, {"filled_quantity", 100}}.dump()});
    json rows = ReconciliationView(sources, report).snapshot(accounts)["items"];
    REQUIRE(rows.size() == 1);
    CHECK(rows[0]["session_id"] == "session-1");
    CHECK(rows[0]["reason"] == "controller_pending");
    CHECK(rows[0]["reported_status"] == "FILLED");
    CHECK(rows[0]["kind"] == "algo_order");
    CHECK(rows[0]["command_id"] == "smart-child");
    CHECK(rows[0]["modes"] == json::array({"G"}));
    // The broker row's report time is kept; the child's submission time does not replace it.
    CHECK(rows[0]["updated_at"] == "2026-10-26T15:00:00Z");
}

TEST_CASE_FIXTURE(ReconciliationFixture, "unavailable sources are explicit and missing databases are not created") {
    broker(sources[0]);
    std::filesystem::remove(sources[1].state_db);
    ngtest::write_file(feedback(sources[1].state_db), "not a database");
    json result = ReconciliationView(sources, report).snapshot(accounts);
    CHECK(result["complete"] == false);
    CHECK(result["items"].size() == 1);
    CHECK(result["sources"][1] == json{{"mode", "H"}, {"feedback", "unavailable"}, {"state", "missing"}});
    CHECK(result["sources"][0] == json{{"mode", "G"}, {"feedback", "available"}, {"state", "available"}});
    CHECK_FALSE(std::filesystem::exists(sources[1].state_db));
    CHECK(result.dump().find(dir.path.string()) == std::string::npos);
    // A missing report database only removes the extra broker details.
    json without_details = ReconciliationView(sources, dir.file("absent.sqlite3")).snapshot(accounts);
    CHECK(without_details["details_available"] == false);
    CHECK(without_details["items"].size() == 1);
    CHECK_FALSE(std::filesystem::exists(dir.file("absent.sqlite3")));
    CHECK(ReconciliationView({}, report).snapshot(accounts)["complete"] == false);
}

TEST_CASE("the order API requires login and refreshes persisted blockers") {
    ReconciliationFixture databases;
    databases.broker(databases.sources[0]);
    OrderUiFixture f("UTC");
    f.config.accounts = databases.accounts;
    f.config.reconciliation_sources = databases.sources;
    f.open();
    CHECK(f.get("/api/orders")->status == 401);
    CHECK(f.login()->status == 200);
    json result = f.report()["unreconciled"];
    CHECK(result["complete"] == true);
    CHECK(result["items"].size() == 1);
    ReconciliationFixture::write(ReconciliationFixture::feedback(databases.sources[0].state_db), "UPDATE feedback_orders SET status='FILLED'");
    CHECK(f.report()["unreconciled"]["items"].empty());
}
