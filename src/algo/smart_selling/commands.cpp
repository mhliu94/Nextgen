#include "algo/smart_selling/commands.h"

#include <cmath>

#include "core/decimal.h"
#include "core/strings.h"
#include "orders/commands.h"

namespace ng::smart {
namespace {

// Reject precision loss at the numeric command contract boundary (_wire_number).
json wire_number(const json& value, const char* name, bool allow_integer) {
    Decimal exact = Decimal::parse(json_to_string(&value).value_or(""));
    if (allow_integer && exact.is_integral()) return json(exact.to_int64());
    double number = exact.to_double();
    if (Decimal::from_double(number) != exact) {
        throw std::invalid_argument(std::string(name) + " cannot be represented exactly by the order command");
    }
    return json(number);
}

}  // namespace

json build_smart_selling_command(const json& action, const AccountDirectory& accounts) {
    std::string account_id = action.at("account_id").get<std::string>();
    std::string symbol = action.at("symbol").get<std::string>();
    std::string type = action.at("type").get<std::string>();
    json command;
    if (type == "submit") {
        json shares = wire_number(action.at("quantity"), "Quantity", true);
        json price = wire_number(action.at("limit_price"), "Limit price", false);
        long long qty = shares.is_number_integer() ? shares.get<long long>() : static_cast<long long>(shares.get<double>());
        command = orders::build_limit_order_command(account_id, symbol, "SELL", qty, price.get<double>(), accounts);
        if (!shares.is_number_integer()) command["qty_shares"] = shares;  // fractional quantities stay exact floats
        command["command_id"] = action.at("client_order_id");
        command["client_order_id"] = action.at("client_order_id");
        command["time_in_force"] = action.at("time_in_force");
        command["ts"] = action.at("send_time");
        command["deadline"] = action.at("deadline");
    } else if (type == "cancel") {
        command = orders::build_cancel_open_orders_command(account_id, accounts, symbol);
    } else {
        throw std::invalid_argument("Only submit/cancel intents translate to trading commands");
    }
    command["parent_id"] = action.at("parent_id");
    command["trading_mode"] = "G";
    return command;
}

}  // namespace ng::smart
