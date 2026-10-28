#include <doctest.h>

#include "core/config.h"
#include "test_helpers.h"

using namespace ngtest;

namespace {

std::string base_config(const std::string& extra = "") {
    return R"({
  "accounts_file": "./accounts.json",
  "symbols": ["tsla", "NVDA", "TSLA", " aehl "],
  "auth": {"users": {"admin": {"password": "admin12345"}, "trader": {"password": "trader12345"}, "lmh": {"password": "x"}}},
  "server": {"host": "0.0.0.0", "port": 8000, "ssl_enabled": false},
  "kafka": {"bootstrap_servers": "127.0.0.1:9092", "market_data_bootstrap_servers": "45.32.121.19:9092"},
  "market_data": {"historical_prices_csv": "../hist.csv", "market_insights_max_levels": 20}
  )" + extra + "}";
}

std::string accounts_json() {
    return R"({"accounts": [
      {"ip_address": "1.2.3.4", "machine_alias": "box1", "numeric_id": 1, "string_id": "A", "broker": "Tiger", "trading_medium": "API", "monitor": true},
      {"numeric_id": 2, "string_id": "B", "broker": "GigaMoney", "trading_medium": "emulator", "broker_id": "G-1"},
      {"numeric_id": 3, "string_id": "H", "broker": "NA", "trading_medium": "API", "hidden": true}
    ]})";
}

}  // namespace

TEST_CASE("load_config applies defaults, env fallbacks and path resolution") {
    TempDir dir("config");
    write_file(dir.file("config.json"), base_config());
    write_file(dir.file("accounts.json"), accounts_json());
    unsetenv("UI_SSL_ENABLED");
    unsetenv("MARKET_INSIGHTS_MAX_LEVELS");
    ng::AppConfig cfg = ng::load_config(dir.file("config.json"));
    CHECK(cfg.accounts_file == (dir.path / "accounts.json").lexically_normal().string());
    CHECK(cfg.kafka.account_details_topic == "account-details");
    CHECK(cfg.kafka.trading_commands_topic == "trading-commands");
    CHECK(cfg.kafka.market_data_topic == "price-books");
    CHECK(cfg.kafka.stock_quotes_topic == "stock-quotes");
    CHECK(cfg.kafka.group_id == "account-dashboard");
    CHECK(cfg.kafka.auto_offset_reset == "latest");
    CHECK(cfg.kafka.market_data_auto_offset_reset == "earliest");
    CHECK(cfg.kafka.producer_acks == "all");
    CHECK(cfg.kafka.producer_enable_idempotence);
    CHECK(cfg.kafka.producer_linger_ms == 5);
    CHECK(cfg.market_data.market_insights_max_levels == 20);
    CHECK(cfg.market_data.historical_prices_csv == (dir.path / "../hist.csv").lexically_normal().string());
    CHECK(cfg.fast_trading.aggression_levels.at(2).book_levels == 3);
    CHECK(cfg.fast_trading.minimum_cycle_seconds_by_medium.at("WINDOWS") == 40.0);
    CHECK(cfg.smart_selling.enabled);
    CHECK(cfg.smart_selling.participation_percentages.at("normal") == "6");
    CHECK(cfg.smart_selling.state_db == "./logs/smart_selling.sqlite3");
    CHECK(cfg.users.at("admin").role == "admin");
    CHECK(cfg.users.at("lmh").password == "x");
    CHECK(cfg.is_privileged("lmh"));
    CHECK_FALSE(cfg.is_privileged("trader"));
    CHECK(ng::load_symbols(cfg) == std::vector<std::string>{"TSLA", "NVDA", "AEHL"});

    ng::AccountDirectory accounts = ng::load_account_metas(cfg);
    REQUIRE(accounts.size() == 2);  // hidden account excluded
    CHECK(accounts.find("A")->monitor);
    CHECK(accounts.find("A")->ip_address == "1.2.3.4");
    CHECK(accounts.find("B")->trading_medium == "EMULATOR");
    CHECK(accounts.find("B")->broker_id == "G-1");
    CHECK_FALSE(accounts.contains("H"));
    CHECK(accounts.ids_by_num_id() == std::vector<std::string>{"A", "B"});
}

TEST_CASE("load_config validation errors match the python messages") {
    TempDir dir("config-errors");
    write_file(dir.file("accounts.json"), accounts_json());
    write_file(dir.file("no-symbols.json"), R"({"accounts_file": "./accounts.json", "auth": {"users": {"admin": {"password": "a"}, "trader": {"password": "b"}}}})");
    CHECK_THROWS_WITH(ng::load_config(dir.file("no-symbols.json")), "Config missing non-empty 'symbols' list.");
    write_file(dir.file("no-pw.json"), R"({"accounts_file": "./accounts.json", "symbols": ["X"], "auth": {"users": {"admin": {"password": "a"}}}})");
    CHECK_THROWS_WITH(ng::load_config(dir.file("no-pw.json")), "Config must provide non-empty auth.users.admin.password and auth.users.trader.password.");
    write_file(dir.file("bad-fast.json"), base_config(R"(, "fast_trading": {"aggression_levels": {"1": {"book_levels": 2, "cycle_seconds": 30}}})"));
    CHECK_THROWS_WITH(ng::load_config(dir.file("bad-fast.json")), "Config fast_trading.aggression_levels must define exactly levels 1, 2, and 3.");
    write_file(dir.file("bad-smart.json"), base_config(R"(, "smart_selling": {"participation_percentages": {"calm": "3"}})"));
    CHECK_THROWS_WITH(ng::load_config(dir.file("bad-smart.json")), "Unknown smart_selling.participation_percentages modes: calm");
    write_file(dir.file("bad-pct.json"), base_config(R"(, "smart_selling": {"participation_percentages": {"normal": "100.01"}})"));
    CHECK_THROWS(ng::load_config(dir.file("bad-pct.json")));
    write_file(dir.file("ssl.json"), base_config(R"(, "server": {"ssl_enabled": true, "ssl_certfile": "missing.crt", "ssl_keyfile": "missing.key"})"));
    CHECK_THROWS_AS(ng::load_config(dir.file("ssl.json")), ng::ConfigError);
}

TEST_CASE("smart selling config normalisation") {
    ng::json raw = ng::json::parse(R"({"participation_percentages": {"urgent": "12.5"}, "instruments": {"us.xyz": {"tick_size": "0.0001"}}, "quote_max_age": 3})");
    ng::SmartSellingConfig cfg = ng::normalize_smart_selling_config(&raw);
    CHECK(cfg.participation_percentages.at("patient") == "3");
    CHECK(cfg.participation_percentages.at("urgent") == "12.5");
    CHECK(cfg.quote_max_age == 3);
    CHECK(cfg.trade_max_age == 15);
    CHECK(cfg.instruments.at("XYZ").at("tick_size") == "0.0001");
    auto rules = cfg.rules_for("XYZ");
    CHECK(rules.at("tick_size") == "0.0001");
    CHECK(rules.at("quantity_step") == "1");
    CHECK(cfg.rules_for("OTHER").at("tick_size") == "0.01");
    ng::json bad = ng::json::parse(R"({"instruments": {"X": {"tick_ladder": []}}})");
    CHECK_THROWS_WITH(ng::normalize_smart_selling_config(&bad), "Smart selling currently requires a constant tick_size");
    ng::json bad_enabled = ng::json::parse(R"({"enabled": "yes"})");
    CHECK_THROWS_WITH(ng::normalize_smart_selling_config(&bad_enabled), "smart_selling.enabled must be boolean");
}

TEST_CASE("account meta parsing rejects duplicates and bad media") {
    ng::json dup = ng::json::parse(R"([{"string_id": "A", "numeric_id": 1, "broker": "T", "trading_medium": "API"}, {"string_id": "A", "numeric_id": 2, "broker": "T", "trading_medium": "API"}])");
    CHECK_THROWS_WITH(ng::parse_account_metas(dup, true, false), "Duplicate string_id in accounts config: A");
    ng::json dupnum = ng::json::parse(R"([{"string_id": "A", "numeric_id": 1, "broker": "T", "trading_medium": "API"}, {"string_id": "B", "numeric_id": 1, "broker": "T", "trading_medium": "API"}])");
    CHECK_THROWS_WITH(ng::parse_account_metas(dupnum, true, false), "Duplicate numeric_id in accounts config: 1");
    ng::json medium = ng::json::parse(R"([{"string_id": "A", "numeric_id": 1, "broker": "T", "trading_medium": "PHONE"}])");
    CHECK_THROWS(ng::parse_account_metas(medium, true, false));
    ng::json inline_accounts = ng::json::parse(R"([{"id": "A", "num_id": 5, "broker": "T"}])");
    auto metas = ng::parse_account_metas(inline_accounts, false, false);
    REQUIRE(metas.size() == 1);
    CHECK(metas[0].trading_medium == "API");
    CHECK(metas[0].num_id == 5);
    ng::json hidden = ng::json::parse(R"([{"string_id": "A", "numeric_id": 1, "broker": "T", "trading_medium": "API", "hidden": true}, {"string_id": "B", "numeric_id": 1, "broker": "T", "trading_medium": "API"}])");
    CHECK(ng::parse_account_metas(hidden, true, false).size() == 1);
    CHECK_THROWS(ng::parse_account_metas(hidden, true, true));  // duplicate numeric id once hidden ones are included
}
