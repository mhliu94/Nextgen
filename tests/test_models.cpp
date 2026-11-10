#include <doctest.h>

#include "model/account.h"
#include "model/account_store.h"
#include "model/market_data.h"
#include "test_helpers.h"

using ng::json;

TEST_CASE("parse_snapshot_obj mirrors the python parser") {
    json payload = json::parse(R"({
      "account_id": " A ", "account_num_id": "3", "cash": "250.5",
      "cash_by_currency": {" usd ": 250.5, "hkd": "900"},
      "available_cash_by_currency": {"USD": 10, "HKD": "nan"},
      "ts": "2026-10-23T19:59:58Z", "trading_enabled": "yes",
      "positions": [{"symbol": "AAPL", "qty": "2", "avg_price": 100.5, "salable_qty": 1}, {"symbol": "MSFT", "qty": 9}]
    })");
    auto snap = ng::parse_snapshot_obj(payload);
    REQUIRE(snap);
    CHECK(snap->account_id == "A");
    CHECK(snap->account_num_id == 3);
    CHECK(snap->cash == 250.5);
    CHECK(snap->cash_by_currency.at("USD") == 250.5);
    CHECK(snap->cash_by_currency.at("HKD") == 900.0);
    CHECK(snap->available_cash_by_currency.at("USD") == 10.0);
    CHECK_FALSE(snap->available_cash_by_currency.count("HKD"));
    CHECK(snap->trading_enabled);
    REQUIRE(snap->positions.size() == 2);
    CHECK(snap->positions[0].available_qty == 1.0);
    CHECK(snap->positions[0].avg_price == 100.5);
    CHECK_FALSE(snap->positions[1].avg_price);
    CHECK_FALSE(snap->positions[1].available_qty);
    CHECK(snap->usd_cash() == 250.5);
    CHECK(snap->available_usd_cash() == 10.0);
    CHECK(snap->ts_epoch() == doctest::Approx(1792785598.0));

    json minimal = json::parse(R"({"account_id": "B"})");
    auto snap2 = ng::parse_snapshot_obj(minimal);
    REQUIRE(snap2);
    CHECK(snap2->cash == 0.0);
    CHECK(snap2->cash_by_currency.at("USD") == 0.0);
    CHECK_FALSE(snap2->trading_enabled);
    CHECK_FALSE(ng::parse_snapshot_obj(json::parse(R"({"account_id": ""})")));
    CHECK_FALSE(ng::parse_snapshot_obj(json::parse(R"({"cash": 1})")));
    CHECK_FALSE(ng::parse_snapshot_obj(json::parse(R"({"account_id": "A", "cash": "abc"})")));
    CHECK_FALSE(ng::parse_snapshot_obj(json::parse(R"({"account_id": "A", "positions": [{"qty": 1}]})")));
    auto round = ng::parse_snapshot_obj(snap->to_json());
    REQUIRE(round);
    CHECK(round->positions.size() == 2);
}

TEST_CASE("account store notifies listeners and tracks versions") {
    ng::AccountStore store;
    int notified = 0;
    store.add_listener([&] { ++notified; });
    ng::AccountSnapshot snap;
    snap.account_id = "A";
    snap.cash = 100;
    store.upsert(snap);
    snap.cash = 200;
    store.upsert(snap);
    CHECK(notified == 2);
    CHECK(store.version() == 2);
    CHECK(store.get("A")->cash == 200);
    CHECK(store.get_all().size() == 1);
    CHECK(store.kafka_last_seen().has_value());
}

TEST_CASE("price book parsing caps from the most aggressive prices") {
    json payload = json::parse(R"({
      "symbol": "aapl", "update_time": 1700000000123456789, "volume": "12345",
      "bid_side": {"levels": [{"prc": 99, "qty": 5}, {"prc": 100, "qty": 3, "order_count": 2}, {"prc": null, "qty": 1}, {"price": 98, "TotalSize": 7}]},
      "offer_side": {"levels": [{"prc": 101, "qty": 4}, {"prc": 100.5, "qty": 6}]}
    })");
    auto book = ng::order_book_from_price_book(payload, 2);
    REQUIRE(book);
    CHECK(book->symbol == "AAPL");
    REQUIRE(book->bids.size() == 2);
    CHECK(book->bids[0].price == 100.0);
    CHECK(book->bids[0].order_count == 2);
    CHECK(book->bids[1].price == 99.0);
    REQUIRE(book->asks.size() == 2);
    CHECK(book->asks[0].price == 100.5);
    CHECK(book->asof_epoch == 1700000000);
    auto row = ng::quote_row_from_price_book(payload);
    REQUIRE(row);
    CHECK(row->last == doctest::Approx(100.25));
    CHECK(row->volume == 12345);
    CHECK(row->asof_epoch == 1700000000);
    CHECK_FALSE(ng::quote_row_from_price_book(json::parse(R"({"symbol": " "})")));
    json one_sided = json::parse(R"({"symbol": "X", "bid_side": {"levels": [{"prc": 5, "qty": 1}]}})");
    CHECK(ng::quote_row_from_price_book(one_sided)->last == 5.0);
}

TEST_CASE("market data store merges rows and caps books") {
    ng::MarketDataStore store;
    json payload = json::parse(R"({"symbol": "X", "update_time": 1700000000000000000, "volume": 10,
      "bid_side": {"levels": [{"prc": 1, "qty": 1}, {"prc": 2, "qty": 1}, {"prc": 3, "qty": 1}]},
      "offer_side": {"levels": [{"prc": 6, "qty": 1}, {"prc": 5, "qty": 1}, {"prc": 4, "qty": 1}]}})");
    store.upsert(*ng::quote_row_from_price_book(payload));
    store.upsert_book(*ng::order_book_from_price_book(payload, 20));
    auto book = store.get_book("X", 2);
    REQUIRE(book.bids.size() == 2);
    CHECK(book.bids[0].price == 3.0);
    CHECK(book.asks[0].price == 4.0);
    CHECK(book.depth_limit == 2);
    CHECK_FALSE(book.error);
    auto missing = store.get_book("Y", 5);
    CHECK(missing.error == "no data yet");
    CHECK(store.get_book_symbols() == std::vector<std::string>{"X"});
    ng::QuoteRow partial;
    partial.symbol = "X";
    partial.short_interest = 12.5;
    store.upsert(partial);
    auto rows = store.get_for_symbols({"X", "Z"});
    CHECK(rows.at("X").last == doctest::Approx(3.5));
    CHECK(rows.at("X").short_interest == 12.5);
    CHECK(rows.at("X").volume == 10);
    CHECK(rows.at("Z").error == "no data yet");
}

TEST_CASE("stock quote parsing and ordering") {
    json healthy = json::parse(R"({"schema_version": 1, "type": "STOCK_QUOTE", "symbol": "aapl", "last": "125.1250", "trade_time": "2026-10-23T19:59:58Z", "observed_at": "2026-10-23T20:00:00Z", "is_delayed": false, "is_halted": false, "healthy": true})");
    auto quote = ng::stock_quote_from_payload(healthy);
    REQUIRE(quote);
    CHECK(quote->symbol == "AAPL");
    CHECK(quote->last == 125.125);
    CHECK(quote->usable());
    json unhealthy = json::parse(R"({"schema_version": 1, "type": "STOCK_QUOTE", "symbol": "AAPL", "last": null, "observed_at": "2026-10-23T20:00:05Z", "healthy": false, "reason": "x"})");
    auto bad = ng::stock_quote_from_payload(unhealthy);
    REQUIRE(bad);
    CHECK_FALSE(bad->usable());
    CHECK(bad->error == "last trade unavailable");
    CHECK_FALSE(ng::stock_quote_from_payload(json::parse(R"({"schema_version": 2, "type": "STOCK_QUOTE", "symbol": "A", "observed_at": "2026-10-23T20:00:05Z"})")));
    CHECK_FALSE(ng::stock_quote_from_payload(json::parse(R"({"schema_version": 1, "type": "PRICE_BOOK", "symbol": "A"})")));
    json delayed = healthy;
    delayed["is_delayed"] = true;
    CHECK_FALSE(ng::stock_quote_from_payload(delayed)->usable());
    json future = healthy;
    future["trade_time"] = "2026-10-23T20:00:05Z";
    CHECK_FALSE(ng::stock_quote_from_payload(future)->usable());

    ng::MarketDataStore store;
    store.upsert_stock_quote(*quote);
    store.upsert_stock_quote(*bad);  // newer unavailable observation clears the price
    CHECK_FALSE(store.get_stock_quotes({"AAPL"}).at("AAPL").usable());
    store.upsert_stock_quote(*quote);  // older observation cannot restore it
    CHECK_FALSE(store.get_stock_quotes({"AAPL"}).at("AAPL").usable());
}
