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
  "market_data": {"market_insights_max_levels": 20}
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
    CHECK(cfg.kafka.market_data_auto_offset_reset == "latest");
    CHECK(cfg.kafka.market_data_replay_records == 100);
    CHECK(cfg.kafka.producer_acks == "all");
    CHECK(cfg.kafka.producer_enable_idempotence);
    CHECK(cfg.kafka.producer_linger_ms == 5);
    CHECK(cfg.market_data.market_insights_max_levels == 20);
    CHECK(cfg.market_data.publisher_config.empty());
    CHECK(cfg.market_data_symbols() == std::vector<std::string>{"TSLA", "NVDA", "AEHL"});
    CHECK(cfg.fast_trading.aggression_levels.at(2).book_levels == 3);
    CHECK(cfg.fast_trading.minimum_cycle_seconds_by_medium.at("WINDOWS") == 40.0);
    CHECK(cfg.smart_selling.enabled);
    CHECK(cfg.smart_selling.participation_percentages.at("normal") == "12");
    CHECK(cfg.smart_selling.participation_percentages.at("ultra") == "30");
    CHECK(cfg.smart_selling.instrument_defaults.at("min_quantity") == "100");
    CHECK(cfg.smart_buying.enabled);
    CHECK(cfg.smart_buying.state_db == "./logs/smart_buying.sqlite3");
    CHECK(cfg.smart_buying.participation_percentages == std::map<std::string, std::string>{{"normal", "8"}, {"patient", "5"}, {"urgent", "15"}});
    CHECK(cfg.reporting.db_path == std::filesystem::absolute("./reports/activity.sqlite3").lexically_normal().string());
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
    CHECK(cfg.participation_percentages.at("patient") == "6");
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

// tests/test_smart_selling_config.py
TEST_CASE("smart selling participation defaults and partial overrides use percentage units") {
    const std::map<std::string, std::string> expected = {{"patient", "6"}, {"normal", "12"}, {"urgent", "20"}, {"ultra", "30"}};
    CHECK(ng::normalize_smart_selling_config(nullptr).participation_percentages == expected);
    for (const char* text : {"{}", R"({"participation_percentages": {}})"}) {
        ng::json raw = ng::json::parse(text);
        CHECK(ng::normalize_smart_selling_config(&raw).participation_percentages == expected);
    }
    ng::json raw = ng::json::parse(R"({"participation_percentages": {"normal": "7.25"}})");
    auto normalized = ng::normalize_smart_selling_config(&raw).participation_percentages;
    auto overridden = expected;
    overridden["normal"] = "7.25";
    CHECK(normalized == overridden);
    CHECK(raw == ng::json::parse(R"({"participation_percentages": {"normal": "7.25"}})"));
}

TEST_CASE("smart selling participation decimals remain exact") {
    ng::json raw = ng::json::parse(R"({"participation_percentages": {"patient": "0.123456789012345678901234",
        "normal": "6.000000000000000000000001", "urgent": 100, "ultra": "27.5"}})");
    CHECK(ng::normalize_smart_selling_config(&raw).participation_percentages ==
          std::map<std::string, std::string>{{"patient", "0.123456789012345678901234"},
                                             {"normal", "6.000000000000000000000001"},
                                             {"urgent", "100"},
                                             {"ultra", "27.5"}});
    ng::json tiny = ng::json::parse(R"({"participation_percentages": {"patient": 1e-24}})");
    CHECK(ng::normalize_smart_selling_config(&tiny).participation_percentages.at("patient") == "0.000000000000000000000001");
}

TEST_CASE("smart selling participation settings require an object with known modes") {
    for (const char* invalid : {"null", "true", "false", "3", R"("3")", "[]", R"(["patient", 3])"}) {
        CAPTURE(std::string(invalid));
        ng::json raw = {{"participation_percentages", ng::json::parse(invalid)}};
        CHECK_THROWS_WITH_AS(ng::normalize_smart_selling_config(&raw), "smart_selling.participation_percentages must be an object",
                             ng::ConfigError);
    }
    for (const char* unknown : {"fast", "Patient", ""}) {
        CAPTURE(std::string(unknown));
        ng::json raw = {{"participation_percentages", {{unknown, 5}}}};
        CHECK_THROWS_WITH_AS(ng::normalize_smart_selling_config(&raw),
                             (std::string("Unknown smart_selling.participation_percentages modes: ") + unknown).c_str(), ng::ConfigError);
    }
}

TEST_CASE("smart selling percentage values reject invalid numbers and precision") {
    const char* invalid_values[] = {"true", "false", "null", R"("")", R"("lots")", R"("6%")", "[]", "{}", R"("NaN")", R"("Infinity")",
                                    R"("-Infinity")", R"("sNaN")", "0", R"("-0")", "-1", R"("100.000000000000000000000001")", "101",
                                    R"("1e-25")", R"("1.234567890123456789012345678901")", R"("1e1000000")"};
    for (const char* mode : {"patient", "normal", "urgent", "ultra"}) {
        for (const char* invalid : invalid_values) {
            CAPTURE(std::string(mode));
            CAPTURE(std::string(invalid));
            ng::json raw = {{"participation_percentages", {{mode, ng::json::parse(invalid)}}}};
            try {
                ng::normalize_smart_selling_config(&raw);
                FAIL("expected a configuration error");
            } catch (const ng::ConfigError& e) {
                CHECK(std::string(e.what()).find(std::string("participation_percentages.") + mode) != std::string::npos);
            }
        }
    }
}

TEST_CASE("smart buying config keeps fixed rates, a 100-share minimum and its own database") {
    ng::SmartSellingConfig defaults = ng::normalize_smart_buying_config(nullptr);
    CHECK(defaults.state_db == "./logs/smart_buying.sqlite3");
    CHECK(defaults.participation_percentages == std::map<std::string, std::string>{{"patient", "5"}, {"normal", "8"}, {"urgent", "15"}});
    CHECK(defaults.instrument_defaults.at("min_quantity") == "100");
    CHECK(defaults.instrument_defaults.at("quantity_step") == "1");
    // Requests to change the rates are ignored; smaller minimums and steps are raised.
    ng::json raw = ng::json::parse(R"({"participation_percentages": {"patient": "50", "ultra": "30"},
        "instrument_defaults": {"min_quantity": "10", "quantity_step": "0.1"}, "state_db": "./state/buying.sqlite3",
        "quote_max_age": 4})");
    ng::SmartSellingConfig cfg = ng::normalize_smart_buying_config(&raw);
    CHECK(cfg.participation_percentages == defaults.participation_percentages);
    CHECK(cfg.instrument_defaults.at("min_quantity") == "100");
    CHECK(cfg.instrument_defaults.at("quantity_step") == "1");
    CHECK(cfg.state_db == "./state/buying.sqlite3");
    CHECK(cfg.quote_max_age == 4);
    ng::json larger = ng::json::parse(R"({"instrument_defaults": {"min_quantity": "200", "quantity_step": "100"}})");
    cfg = ng::normalize_smart_buying_config(&larger);
    CHECK(cfg.instrument_defaults.at("min_quantity") == "200");
    CHECK(cfg.instrument_defaults.at("quantity_step") == "100");
    ng::json not_object = ng::json::array();
    CHECK_THROWS_WITH_AS(ng::normalize_smart_buying_config(&not_object), "smart_buying must be an object", ng::ConfigError);

    TempDir dir("config-buying");
    write_file(dir.file("accounts.json"), accounts_json());
    write_file(dir.file("same.json"), base_config(R"(, "smart_selling": {"state_db": "./state/smart.sqlite3"},
        "smart_buying": {"state_db": "state/../state/smart.sqlite3"})"));
    CHECK_THROWS_WITH_AS(ng::load_config(dir.file("same.json")), "Smart buying and Smart selling require separate state databases",
                         ng::ConfigError);
    write_file(dir.file("ok.json"), base_config(R"(, "smart_buying": {"state_db": "state/buying.sqlite3", "enabled": false})"));
    ng::AppConfig loaded = ng::load_config(dir.file("ok.json"));
    CHECK(loaded.smart_buying.state_db == (dir.path / "state" / "buying.sqlite3").string());
    CHECK_FALSE(loaded.smart_buying.enabled);
    CHECK(loaded.smart_selling.enabled);
}

TEST_CASE("reporting config resolves a persistent database path") {
    unsetenv("REPORTING_DB_PATH");
    TempDir dir("config-reporting");
    write_file(dir.file("accounts.json"), accounts_json());
    write_file(dir.file("relative.json"), base_config(R"(, "reporting": {"db_path": "state/activity.sqlite3"})"));
    CHECK(ng::load_config(dir.file("relative.json")).reporting.db_path == (dir.path / "state" / "activity.sqlite3").string());
    write_file(dir.file("absolute.json"), base_config(R"(, "reporting": {"db_path": "/var/tmp/ng/activity.sqlite3"})"));
    CHECK(ng::load_config(dir.file("absolute.json")).reporting.db_path == "/var/tmp/ng/activity.sqlite3");
    for (const char* invalid : {R"("reports.sqlite3")", "[]", R"({"db_path": "x", "extra": 1})", R"({"path": "x"})"}) {
        CAPTURE(std::string(invalid));
        write_file(dir.file("bad.json"), base_config(std::string(R"(, "reporting": )") + invalid));
        CHECK_THROWS_WITH_AS(ng::load_config(dir.file("bad.json")), "reporting must be an object containing only db_path.",
                             ng::ConfigError);
    }
    for (const char* invalid : {R"("")", R"("   ")", R"(":memory:")", "7", "true", "[]"}) {
        CAPTURE(std::string(invalid));
        write_file(dir.file("bad.json"), base_config(std::string(R"(, "reporting": {"db_path": )") + invalid + "}"));
        CHECK_THROWS_WITH_AS(ng::load_config(dir.file("bad.json")), "reporting.db_path must be a persistent file path.",
                             ng::ConfigError);
    }
    // The environment override wins over the file and is resolved from the working directory.
    setenv("REPORTING_DB_PATH", "/var/tmp/ng/override.sqlite3", 1);
    CHECK(ng::load_config(dir.file("relative.json")).reporting.db_path == "/var/tmp/ng/override.sqlite3");
    unsetenv("REPORTING_DB_PATH");
}

// tests/test_market_data_replay.py (configuration part)
TEST_CASE("market data replay window configuration") {
    unsetenv("KAFKA_MARKET_DATA_REPLAY_RECORDS");
    unsetenv("KAFKA_MARKET_DATA_AUTO_OFFSET_RESET");
    TempDir dir("config-replay");
    write_file(dir.file("accounts.json"), accounts_json());
    auto load = [&](const std::string& kafka) {
        write_file(dir.file("config.json"),
                   R"({"accounts_file": "./accounts.json", "symbols": ["AAPL"],
                       "auth": {"users": {"admin": {"password": "test"}, "trader": {"password": "test"}}}, "kafka": )" +
                       kafka + "}");
        return ng::load_config(dir.file("config.json")).kafka;
    };
    ng::KafkaConfig defaults = load("{}");
    CHECK(defaults.market_data_replay_records == 100);
    CHECK(defaults.market_data_auto_offset_reset == "latest");

    setenv("KAFKA_MARKET_DATA_REPLAY_RECORDS", "250", 1);
    CHECK(load("{}").market_data_replay_records == 250);
    CHECK(load(R"({"market_data_replay_records": 0})").market_data_replay_records == 0);
    unsetenv("KAFKA_MARKET_DATA_REPLAY_RECORDS");

    for (const char* value : {"-1", R"("-1")", "true", "1.5", "null", R"("")", R"("earliest")", R"("10.5")"}) {
        CAPTURE(std::string(value));
        CHECK_THROWS_WITH_AS(load(std::string(R"({"market_data_replay_records": )") + value + "}"),
                             "Config kafka.market_data_replay_records must be a non-negative integer.", ng::ConfigError);
    }
    CHECK(load(R"({"market_data_replay_records": "25"})").market_data_replay_records == 25);
    // Only integers and integer strings are accepted (a JSON float is rejected even when integral).
    CHECK_THROWS_AS(load(R"({"market_data_replay_records": 7.0})"), ng::ConfigError);
    setenv("KAFKA_MARKET_DATA_REPLAY_RECORDS", "-100", 1);
    CHECK_THROWS_WITH_AS(load("{}"), "Config kafka.market_data_replay_records must be a non-negative integer.", ng::ConfigError);
    unsetenv("KAFKA_MARKET_DATA_REPLAY_RECORDS");
}

// tests/test_live_market_data_config.py
TEST_CASE("market tab follows publisher symbols without changing trading symbols") {
    TempDir dir("config-publisher");
    write_file(dir.file("publisher.json"), R"({"symbols": [" aehl ", "AAPL", "AEHL"]})");
    write_file(dir.file("ui.json"), R"({"accounts": [{"string_id": "test", "numeric_id": 1, "broker": "Tiger"}],
        "symbols": ["MSFT", "TSLA"], "market_data": {"publisher_config": "publisher.json"},
        "auth": {"users": {"admin": {"password": "test"}, "trader": {"password": "test"}}}})");
    ng::AppConfig config = ng::load_config(dir.file("ui.json"));
    CHECK(config.market_data.symbols == std::vector<std::string>{"AEHL", "AAPL"});
    CHECK(config.market_data_symbols() == std::vector<std::string>{"AEHL", "AAPL"});
    CHECK(config.market_data.publisher_config == dir.file("publisher.json"));
    CHECK(config.symbols == std::vector<std::string>{"MSFT", "TSLA"});
    write_file(dir.file("publisher.json"), R"({"symbols": []})");
    try {
        ng::load_config(dir.file("ui.json"));
        FAIL("expected a configuration error");
    } catch (const ng::ConfigError& e) {
        CHECK(std::string(e.what()).find("non-empty symbols list") != std::string::npos);
    }
}

TEST_CASE("page entitlements are limited to the exact lmh user") {
    TempDir dir("config-entitlements");
    write_file(dir.file("config.json"), base_config());
    write_file(dir.file("accounts.json"), accounts_json());
    ng::AppConfig cfg = ng::load_config(dir.file("config.json"));
    for (const char* allowed : {"lmh"}) {
        CHECK(cfg.can_access_admin_control(allowed));
        CHECK(cfg.can_access_market_data(allowed));
    }
    for (const char* denied : {"admin", "trader", "LMH", " lmh", "lmh ", "", "lmh2"}) {
        CAPTURE(std::string(denied));
        CHECK_FALSE(cfg.can_access_admin_control(denied));
        CHECK_FALSE(cfg.can_access_market_data(denied));
    }
    // Hidden accounts stay out of the trading directory but are available for snapshots.
    CHECK(ng::load_account_metas(cfg).size() == 2);
    ng::AccountDirectory all = ng::load_account_metas(cfg, true);
    CHECK(all.size() == 3);
    CHECK(all.contains("H"));
}
