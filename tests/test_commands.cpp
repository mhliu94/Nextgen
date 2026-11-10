#include <doctest.h>

#include <set>
#include "core/time_util.h"
#include "core/strings.h"

#include "orders/commands.h"
#include "test_helpers.h"

using namespace ng::orders;
using ng::json;

namespace {
ng::AccountDirectory accounts() {
    auto a = ngtest::meta("A", 1, "Tiger");
    a.broker_id = "TG-1";
    return ngtest::directory({a, ngtest::meta("B", 2, "GigaMoney", "EMULATOR")});
}
OrderMessages om() { return {"invalid_account", "invalid_symbol", "invalid_side", "both", "neither", "shares_positive", "dollars_positive"}; }
QuickMessages qm() { return {"invalid_account", "invalid_symbol", "invalid_side", "dollars_positive", "no_last", "too_low"}; }
LimitMessages lm() { return {"invalid_account", "invalid_symbol", "invalid_side", "shares_positive", "price_positive", "no_last"}; }
AlgoMessages am() {
    return {"invalid_mode", "invalid_symbol", "invalid_account", "config_required", "price_positive", "accounts_required",
            "dup {account}", "levels", "pct", "cycle"};
}
}  // namespace

TEST_CASE("market order validation and command shape") {
    auto dir = accounts();
    auto v = validate_order_inputs("A", " aapl ", "BUY", std::string("100"), std::nullopt, dir, om());
    REQUIRE(v.ok());
    const json& cmd = *v.command;
    CHECK(cmd["type"] == "MARKET_ORDER");
    CHECK(cmd["account_id"] == "A");
    CHECK(cmd["symbol"] == "AAPL");
    CHECK(cmd["side"] == "BUY");
    CHECK(cmd["qty_shares"] == 100);
    CHECK(cmd["broker"] == "Tiger");
    CHECK(cmd["broker_id"] == "TG-1");
    CHECK_FALSE(cmd.contains("notional_usd"));
    CHECK(ng::starts_with(cmd["command_id"].get<std::string>(), "cmd_"));
    CHECK(ng::parse_iso8601(cmd["ts"].get<std::string>()));

    CHECK(validate_order_inputs("Z", "AAPL", "BUY", std::string("1"), std::nullopt, dir, om()).error == "invalid_account");
    CHECK(validate_order_inputs("A", " ", "BUY", std::string("1"), std::nullopt, dir, om()).error == "invalid_symbol");
    CHECK(validate_order_inputs("A", "AAPL", "HOLD", std::string("1"), std::nullopt, dir, om()).error == "invalid_side");
    CHECK(validate_order_inputs("A", "AAPL", "BUY", std::string("1"), std::string("5"), dir, om()).error == "both");
    CHECK(validate_order_inputs("A", "AAPL", "BUY", std::nullopt, std::nullopt, dir, om()).error == "neither");
    CHECK(validate_order_inputs("A", "AAPL", "BUY", std::string("0"), std::nullopt, dir, om()).error == "shares_positive");
    CHECK(validate_order_inputs("A", "AAPL", "BUY", std::string("1.5"), std::nullopt, dir, om()).error == "shares_positive");
    CHECK(validate_order_inputs("A", "AAPL", "BUY", std::nullopt, std::string("nan"), dir, om()).error == "dollars_positive");
    auto notional = validate_order_inputs("A", "AAPL", "SELL", std::nullopt, std::string("2500.5"), dir, om());
    REQUIRE(notional.ok());
    CHECK((*notional.command)["notional_usd"] == 2500.5);
    CHECK_FALSE(notional.command->contains("qty_shares"));
}

TEST_CASE("quick order floors dollars to whole shares") {
    auto dir = accounts();
    auto v = validate_quick_order_inputs("A", "AAPL", "BUY", std::string("10000"), 123.45, dir, qm());
    REQUIRE(v.ok());
    CHECK((*v.command)["qty_shares"] == 81);
    CHECK(validate_quick_order_inputs("A", "AAPL", "BUY", std::string("100"), 123.45, dir, qm()).error == "too_low");
    CHECK(validate_quick_order_inputs("A", "AAPL", "BUY", std::string("100"), std::nullopt, dir, qm()).error == "no_last");
    CHECK(validate_quick_order_inputs("A", "AAPL", "BUY", std::string("100"), 0.0, dir, qm()).error == "no_last");
    CHECK(validate_quick_order_inputs("A", "AAPL", "BUY", std::string("-1"), 10.0, dir, qm()).error == "dollars_positive");
}

TEST_CASE("limit order uses manual price or percent through market") {
    auto dir = accounts();
    auto manual = validate_limit_order_inputs("A", "AAPL", "BUY", std::string("100"), std::string("182.5"), std::string("5"), 100.0, dir, lm());
    REQUIRE(manual.ok());
    CHECK((*manual.command)["type"] == "LIMIT_ORDER");
    CHECK((*manual.command)["limit_price"] == 182.5);
    auto through_buy = validate_limit_order_inputs("A", "AAPL", "BUY", std::string("100"), std::string(""), std::string("5"), 100.0, dir, lm());
    REQUIRE(through_buy.ok());
    CHECK((*through_buy.command)["limit_price"] == 105.0);
    auto through_sell = validate_limit_order_inputs("A", "AAPL", "SELL", std::string("100"), std::nullopt, std::string("10"), 123.4567, dir, lm());
    REQUIRE(through_sell.ok());
    CHECK((*through_sell.command)["limit_price"].get<double>() == doctest::Approx(111.111).epsilon(1e-9));
    CHECK(validate_limit_order_inputs("A", "AAPL", "BUY", std::string("100"), std::nullopt, std::string("5"), std::nullopt, dir, lm()).error == "no_last");
    CHECK(validate_limit_order_inputs("A", "AAPL", "BUY", std::string("100"), std::nullopt, std::nullopt, 100.0, dir, lm()).error == "price_positive");
    CHECK(validate_limit_order_inputs("A", "AAPL", "BUY", std::string("x"), std::string("1"), std::nullopt, 100.0, dir, lm()).error == "shares_positive");
    CHECK(validate_limit_order_inputs("A", "AAPL", "BUY", std::string("100"), std::string("inf"), std::nullopt, 100.0, dir, lm()).error == "price_positive");
    auto fok = build_limit_order_fok_command("A", "AAPL", "BUY", 10, 5.0, dir);
    CHECK(fok["type"] == "LIMIT_ORDER_FOK");
    CHECK(fok["time_in_force"] == "FOK");
    CHECK(fok["cancel_unfilled"] == true);
}

TEST_CASE("cancel, fx, trading status and algo stop commands") {
    auto dir = accounts();
    auto cancel = validate_cancel_open_orders_inputs("A", std::string(" aapl "), dir, "invalid_account");
    REQUIRE(cancel.ok());
    CHECK((*cancel.command)["type"] == "CANCEL_OPEN_ORDERS");
    CHECK((*cancel.command)["symbol"] == "AAPL");
    auto cancel_all = validate_cancel_open_orders_inputs("A", std::string(""), dir, "invalid_account");
    CHECK_FALSE(cancel_all.command->contains("symbol"));
    CHECK(validate_cancel_open_orders_inputs("Q", std::nullopt, dir, "invalid_account").error == "invalid_account");

    FxMessages fm{"invalid_account", "invalid_currency", "same", "amount"};
    auto fx = validate_currency_conversion_inputs("A", std::string("hkd"), std::string("usd"), std::string("2000"), dir, fm);
    REQUIRE(fx.ok());
    CHECK((*fx.command)["type"] == "CURRENCY_CONVERSION");
    CHECK((*fx.command)["source_currency"] == "HKD");
    CHECK((*fx.command)["source_amount"] == 2000.0);
    CHECK(validate_currency_conversion_inputs("B", std::string("HKD"), std::string("USD"), std::string("1"), dir, fm).error == "invalid_account");
    CHECK(validate_currency_conversion_inputs("A", std::string("EUR"), std::string("USD"), std::string("1"), dir, fm).error == "invalid_currency");
    CHECK(validate_currency_conversion_inputs("A", std::string("USD"), std::string("USD"), std::string("1"), dir, fm).error == "same");
    CHECK(validate_currency_conversion_inputs("A", std::string("USD"), std::string("HKD"), std::string("0"), dir, fm).error == "amount");

    TradingStatusMessages tm{"invalid_account", "invalid_status"};
    auto ts = validate_trading_status_inputs(" A ", "Enabled", dir, tm);
    REQUIRE(ts.ok());
    CHECK((*ts.command)["type"] == "SET_TRADING_ENABLED");
    CHECK((*ts.command)["trading_enabled"] == true);
    CHECK((*validate_trading_status_inputs("A", "off", dir, tm).command)["trading_enabled"] == false);
    CHECK(validate_trading_status_inputs("A", "maybe", dir, tm).error == "invalid_status");

    auto stop = build_algo_stop_command(std::string("  risk limit hit "));
    CHECK(stop["type"] == "STOP_ALGO_TRADING");
    CHECK(stop["scope"] == "ALL");
    CHECK(stop["reason"] == "risk limit hit");
    CHECK_FALSE(build_algo_stop_command(std::string("  ")).contains("reason"));
}

TEST_CASE("fast trading configuration parsing") {
    auto dir = accounts();
    json cfg = json::parse(R"({"price_limit": "10.5", "account_ids": ["A", "B"], "book_levels": 3, "target_percentage": "50", "cycle_seconds": 15.0, "test_mode": "yes"})");
    auto v = validate_algo_start_inputs("E", " aapl ", cfg, dir, am());
    REQUIRE(v.ok());
    const json& cmd = *v.command;
    CHECK(cmd["type"] == "START_ALGO_TRADING");
    CHECK(cmd["trading_mode"] == "E");
    CHECK(cmd["symbol"] == "AAPL");
    CHECK(cmd["fast_trading_price_limit"] == 10.5);
    CHECK(cmd["fast_trading_account_ids"] == json::array({"A", "B"}));
    CHECK(cmd["fast_trading_book_levels"] == 3);
    CHECK(cmd["fast_trading_target_percentage"] == 50);
    CHECK(cmd["fast_trading_cycle_seconds"] == 15);
    CHECK(cmd["fast_trading_test_mode"] == true);
    // JSON string form (from the HTML form).
    auto from_string = validate_algo_start_inputs("F", "AAPL", json(cfg.dump()), dir, am());
    CHECK(from_string.ok());
    CHECK(validate_algo_start_inputs("G", "AAPL", cfg, dir, am()).error == "invalid_mode");
    CHECK(validate_algo_start_inputs("A", "AAPL", cfg, dir, am()).error == "invalid_mode");
    CHECK(validate_algo_start_inputs("E", "AAPL", json(nullptr), dir, am()).error == "config_required");
    CHECK(validate_algo_start_inputs("E", "AAPL", json("not json"), dir, am()).error == "config_required");
    json bad = cfg;
    bad["price_limit"] = 0;
    CHECK(validate_algo_start_inputs("E", "AAPL", bad, dir, am()).error == "price_positive");
    bad = cfg;
    bad["account_ids"] = json::array({"A", "A"});
    CHECK(validate_algo_start_inputs("E", "AAPL", bad, dir, am()).error == "dup A");
    bad = cfg;
    bad["account_ids"] = json::array({"Z"});
    CHECK(validate_algo_start_inputs("E", "AAPL", bad, dir, am()).error == "invalid_account");
    bad = cfg;
    bad["book_levels"] = 6;
    CHECK(validate_algo_start_inputs("E", "AAPL", bad, dir, am()).error == "levels");
    bad = cfg;
    bad["target_percentage"] = 55;
    CHECK(validate_algo_start_inputs("E", "AAPL", bad, dir, am()).error == "pct");
    bad = cfg;
    bad["cycle_seconds"] = 7;
    CHECK(validate_algo_start_inputs("E", "AAPL", bad, dir, am()).error == "cycle");

    json body = json::parse(R"({"trading_mode": "E", "symbol": "AAPL", "fast_trading_price_limit": 10, "fast_trading_account_ids": ["A"], "book_levels": 1, "fast_trading_target_percentage": 100, "fast_trading_cycle_seconds": 5})");
    json extracted = extract_fast_trading_config(body);
    CHECK(extracted["price_limit"] == 10);
    CHECK(extracted["book_levels"] == 1);
    CHECK(extract_fast_trading_config(json::parse(R"({"trading_mode": "E"})")).is_null());
    CHECK(extract_fast_trading_config(json::parse(R"({"fast_trading_config": "raw"})")) == json("raw"));
    json nested = json::parse(R"({"fast_trading_config": {"price_limit": 3}, "fast_trading_price_limit": 9, "account_ids": ["A"]})");
    json merged = extract_fast_trading_config(nested);
    CHECK(merged["price_limit"] == 3);
    CHECK(merged["account_ids"] == json::array({"A"}));
}

// tests/test_orders.py: optional total target position and the percentage choices.
TEST_CASE("optional total target position is validated for both modes") {
    auto dir = accounts();
    AlgoMessages messages = am();
    messages.fast_total_target_position_invalid = "invalid total target position";
    json valid = {{"price_limit", 10}, {"account_ids", json::array({"A"})}, {"book_levels", 2}, {"target_percentage", 100},
                  {"cycle_seconds", 30}};
    for (const char* mode : {"E", "F"}) {
        struct Accepted { json value; std::optional<long long> expected; };
        for (const Accepted& item : {Accepted{0, 0}, Accepted{1000, 1000}, Accepted{1000.0, 1000}, Accepted{" 1000 ", 1000},
                                     Accepted{nullptr, std::nullopt}, Accepted{"", std::nullopt}, Accepted{"  ", std::nullopt}}) {
            CAPTURE(std::string(mode));
            CAPTURE(item.value.dump());
            json config = valid;
            config["total_target_position"] = item.value;
            auto v = validate_algo_start_inputs(mode, "AAPL", config, dir, messages);
            REQUIRE(v.ok());
            if (item.expected) CHECK((*v.command)["fast_trading_total_target_position"] == *item.expected);
            else CHECK_FALSE(v.command->contains("fast_trading_total_target_position"));
        }
        const double two_pow_53 = 9007199254740992.0;
        for (const json& invalid : {json(-1), json(1.5), json(true), json(false), json::array(), json::object(), json("bad"),
                                    json("NaN"), json("Infinity"), json(two_pow_53)}) {
            CAPTURE(std::string(mode));
            CAPTURE(invalid.dump());
            json config = valid;
            config["total_target_position"] = invalid;
            auto v = validate_algo_start_inputs(mode, "AAPL", config, dir, messages);
            CHECK_FALSE(v.ok());
            CHECK(v.error == "invalid total target position");
        }
    }
    // The largest exactly representable share count is accepted.
    json config = valid;
    config["total_target_position"] = 9007199254740991LL;
    CHECK(validate_algo_start_inputs("E", "AAPL", config, dir, messages).ok());
    CHECK(parse_total_target_position(json("1e3")) == 1000);
    CHECK_THROWS_WITH_AS(parse_total_target_position(json("1000.5")), kFastTotalTargetPositionError, std::invalid_argument);
}

TEST_CASE("fast settings accept every published choice and reject the rest") {
    auto dir = accounts();
    json valid = {{"price_limit", 10}, {"account_ids", json::array({"A"})}, {"book_levels", 2}, {"target_percentage", 100},
                  {"cycle_seconds", 30}};
    struct Field { const char* name; std::vector<json> invalid; const char* error; std::vector<int> choices; const char* command_key; };
    for (const Field& field :
         {Field{"book_levels", {0, 6, 1.5}, "levels", {1, 2, 3, 4, 5}, "fast_trading_book_levels"},
          Field{"target_percentage", {0, 5, 25, 110, 130, 140, 151, 10.5, 120.5, 150.5}, "pct",
                {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 120, 150}, "fast_trading_target_percentage"},
          Field{"cycle_seconds", {0, 1, 20, 90, 5.5}, "cycle", {5, 10, 15, 30, 60}, "fast_trading_cycle_seconds"}}) {
        json missing = valid;
        missing.erase(field.name);
        CHECK(validate_algo_start_inputs("E", "AAPL", missing, dir, am()).error == field.error);
        for (const json& invalid : field.invalid) {
            CAPTURE(std::string(field.name));
            CAPTURE(invalid.dump());
            json config = valid;
            config[field.name] = invalid;
            CHECK(validate_algo_start_inputs("E", "AAPL", config, dir, am()).error == field.error);
        }
        for (const char* mode : {"E", "F"}) {
            for (int choice : field.choices) {
                json config = valid;
                config[field.name] = choice;
                auto v = validate_algo_start_inputs(mode, "AAPL", config, dir, am());
                REQUIRE(v.ok());
                CHECK((*v.command)[field.command_key] == choice);
            }
        }
    }
    // Booleans are never numbers for these controls.
    for (const char* name : {"price_limit", "book_levels", "target_percentage", "cycle_seconds"}) {
        json config = valid;
        config[name] = true;
        CHECK_FALSE(validate_algo_start_inputs("E", "AAPL", config, dir, am()).ok());
    }
}

// tests/test_admin_control.py: BrokerOrdersValidationTests.
namespace {
ng::AccountDirectory broker_metas() { return ngtest::directory({ngtest::meta("A", 2, "Tiger"), ngtest::meta("B", 1, "Other")}); }
Validated validate_broker(const std::string& trading_date = "2026-11-01", std::vector<std::string> selected = {"A"},
                          std::optional<ng::AccountDirectory> metas = std::nullopt) {
    return validate_broker_orders_inputs(trading_date, selected, metas ? *metas : broker_metas(),
                                         {"invalid date", "future date", "invalid account", "pick accounts"},
                                         std::string("2026-11-09"));
}
}  // namespace

TEST_CASE("broker orders contract deduplicates and orders the selected accounts") {
    auto v = validate_broker("2026-11-01", {"A", "B", "A"});
    REQUIRE(v.ok());
    const json& command = *v.command;
    std::set<std::string> keys;
    for (auto it = command.begin(); it != command.end(); ++it) keys.insert(it.key());
    CHECK(keys == std::set<std::string>{"type", "command_id", "ts", "trading_date", "timezone", "account_ids"});
    CHECK(command["type"] == "GET_BROKER_ORDERS");
    CHECK(command["account_ids"] == json::array({"B", "A"}));  // by numeric UI id
    CHECK(command["trading_date"] == "2026-11-01");
    CHECK(command["timezone"] == "America/New_York");
    auto ts = ng::parse_iso8601(command["ts"].get<std::string>());
    REQUIRE(ts);
    CHECK(ts->has_offset);
    std::string id = command["command_id"].get<std::string>();
    CHECK(ng::starts_with(id, "broker_orders_"));
    CHECK(id.size() == std::string("broker_orders_").size() + 32);
    CHECK(validate_broker().command->at("command_id") != command["command_id"]);
}

TEST_CASE("broker orders all selection expands to configured accounts without a wildcard") {
    for (const std::vector<std::string>& selected : {std::vector<std::string>{"__ALL__"}, std::vector<std::string>{"__ALL__", "A"}}) {
        auto v = validate_broker("2026-11-01", selected);
        REQUIRE(v.ok());
        CHECK((*v.command)["account_ids"] == json::array({"B", "A"}));
    }
}

TEST_CASE("broker orders reject invalid or future dates") {
    for (const char* day : {"", "2026-02-29", "2026-11-31", "20261101", "2026-W45-1", "2026-1-1", "2026-11-01T00:00:00Z",
                            " 2026-11-01", "2026-11-01 ", "0000-01-01", "2026-13-01", "2026-00-10", "2026-11-00"}) {
        CAPTURE(std::string(day));
        auto v = validate_broker(day);
        CHECK_FALSE(v.ok());
        CHECK(v.error == "invalid date");
    }
    auto future = validate_broker("2026-11-10");
    CHECK_FALSE(future.ok());
    CHECK(future.error == "future date");
    CHECK(validate_broker("2024-02-29").ok());
    CHECK(validate_broker("2026-11-09").ok());
    CHECK(validate_broker("1999-12-31").ok());
}

TEST_CASE("broker orders with empty or unknown accounts never build a command") {
    auto none = validate_broker("2026-11-01", {});
    CHECK_FALSE(none.ok());
    CHECK(none.error == "pick accounts");
    auto empty_directory = validate_broker("2026-11-01", {"__ALL__"}, ng::AccountDirectory());
    CHECK_FALSE(empty_directory.ok());
    CHECK(empty_directory.error == "pick accounts");
    for (const std::vector<std::string>& selected :
         {std::vector<std::string>{"A", "unknown"}, std::vector<std::string>{"__ALL__", "unknown"}, std::vector<std::string>{""}}) {
        auto v = validate_broker("2026-11-01", selected);
        CHECK_FALSE(v.ok());
        CHECK(v.error == "invalid account");
    }
}

TEST_CASE("broker orders today follows the New York calendar") {
    std::string today = broker_orders_today();
    CHECK(today.size() == 10);
    CHECK(today[4] == '-');
    CHECK(today[7] == '-');
    // Today is accepted, tomorrow is not (whatever the wall clock is).
    CHECK(validate_broker_orders_inputs(today, {"A"}, broker_metas(), {"d", "f", "a", "p"}).ok());
}

TEST_CASE("algorithm account conflict message lists the accounts") {
    CHECK(algo_account_conflict_error({"A"}) ==
          "Account(s) A already belong to an algorithm session; stop it and await outstanding order confirmations before "
          "starting another");
    CHECK(algo_account_conflict_error({"B", "A"}).rfind("Account(s) B, A already belong", 0) == 0);
}
