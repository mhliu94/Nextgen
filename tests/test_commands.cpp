#include <doctest.h>
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
