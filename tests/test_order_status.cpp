#include <doctest.h>

#include "algo/smart_selling/order_status.h"

using ng::json;
using namespace ng::smart;

namespace {
json valid_snapshot() {
    return json::parse(R"({
      "schema_version": 1, "account_id": "GIGA-HWK", "account_num_id": 3, "order_id": "broker-order-12345", "sequence": 2,
      "created_at": "2026-09-27T09:30:00-04:00", "updated_at": "2026-09-27T09:30:05-04:00", "symbol": "AAPL", "side": "BUY",
      "order_type": "LIMIT", "status": "PARTIALLY_FILLED", "quantity": 100, "filled_quantity": 40, "average_fill_price": 200.12,
      "limit_price": 200.5, "currency": "USD", "time_in_force": "DAY", "command_id": "ktrader-command-12345",
      "message": "40 shares filled; 60 remaining"})");
}
}  // namespace

TEST_CASE("order status v1 snapshots validate like the JSON schema") {
    OrderEvent event = parse_order_payload(valid_snapshot());
    CHECK(event.created_at < event.updated_at);
    CHECK(event.created_at_iso == "2026-09-27T13:30:00Z");
    CHECK(parse_order_message(valid_snapshot().dump()).payload["order_id"] == "broker-order-12345");

    auto rejects = [](const std::function<void(json&)>& mutate, const char* note) {
        json snapshot = valid_snapshot();
        mutate(snapshot);
        INFO(note);
        CHECK_THROWS_AS(parse_order_payload(snapshot), std::invalid_argument);
    };
    rejects([](json& s) { s["extra"] = 1; }, "additional property");
    rejects([](json& s) { s.erase("sequence"); }, "missing required");
    rejects([](json& s) { s["schema_version"] = 2; }, "schema version");
    rejects([](json& s) { s["account_id"] = " A"; }, "leading whitespace");
    rejects([](json& s) { s["symbol"] = "A B"; }, "symbol whitespace");
    rejects([](json& s) { s["side"] = "SHORT"; }, "side enum");
    rejects([](json& s) { s["status"] = "WORKING"; }, "status enum");
    rejects([](json& s) { s["sequence"] = -1; }, "negative sequence");
    rejects([](json& s) { s["created_at"] = "2026-09-27"; }, "date only");
    rejects([](json& s) { s["created_at"] = "2026-09-27T09:30:00"; }, "missing offset");
    rejects([](json& s) { s["updated_at"] = "2026-09-27T09:29:00-04:00"; }, "updated before created");
    rejects([](json& s) { s["filled_quantity"] = 150; }, "filled exceeds quantity");
    rejects([](json& s) { s["status"] = "FILLED"; }, "filled status with partial fill");
    rejects([](json& s) { s["status"] = "PARTIALLY_FILLED"; s["filled_quantity"] = 100; }, "partial with complete fill");
    rejects([](json& s) { s["status"] = "PARTIALLY_FILLED"; s["filled_quantity"] = 0; }, "partial with zero fill");
    rejects([](json& s) { s["quantity"] = nullptr; }, "null quantity without notional");
    rejects([](json& s) { s.erase("limit_price"); }, "limit order without limit price");
    rejects([](json& s) { s["order_type"] = "STOP"; }, "stop without stop price");
    rejects([](json& s) { s["currency"] = "usd"; }, "currency pattern");
    rejects([](json& s) { s["message"] = std::string(2001, 'x'); }, "message too long");
    rejects([](json& s) { s["average_fill_price"] = -1; }, "negative price");

    json ok = valid_snapshot();
    ok["quantity"] = nullptr;
    ok["notional"] = 2500;
    ok["status"] = "NEW";
    ok["filled_quantity"] = 0;
    ok["average_fill_price"] = nullptr;
    CHECK_NOTHROW(parse_order_payload(ok));
    json filled = valid_snapshot();
    filled["status"] = "FILLED";
    filled["filled_quantity"] = 100;
    CHECK_NOTHROW(parse_order_payload(filled));
    json stop = valid_snapshot();
    stop["order_type"] = "STOP_LIMIT";
    stop["stop_price"] = 199.0;
    CHECK_NOTHROW(parse_order_payload(stop));
    json integral_float = valid_snapshot();
    integral_float["sequence"] = 2.0;
    CHECK_NOTHROW(parse_order_payload(integral_float));
    std::string huge(70000, ' ');
    CHECK_THROWS_WITH(parse_order_message(huge), "Order snapshot exceeds 64 KiB.");
    CHECK_THROWS_AS(parse_order_message("{not json"), std::invalid_argument);
    CHECK_THROWS_AS(parse_order_message("[1,2]"), std::invalid_argument);
}
