#include "algo/smart_selling/order_status.h"

#include <cmath>
#include <set>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng::smart {
namespace {

[[noreturn]] void fail(const std::string& field, const char* rule) {
    throw std::invalid_argument("Invalid " + field + ": schema rule " + rule + ".");
}

bool is_integer_value(const json& v) {
    if (v.is_number_integer() || v.is_number_unsigned()) return true;
    if (v.is_number_float()) {
        double d = v.get<double>();
        return std::isfinite(d) && std::floor(d) == d;
    }
    return false;
}

long long integer_value(const json& v) {
    if (v.is_number_float()) return static_cast<long long>(v.get<double>());
    if (v.is_number_unsigned()) return static_cast<long long>(v.get<unsigned long long>());
    return v.get<long long>();
}

void require_string(const json& payload, const char* key, std::size_t min_len, std::size_t max_len, bool required) {
    const json* v = json_find(payload, key);
    if (!v) {
        if (required) fail("snapshot", "required");
        return;
    }
    if (!v->is_string()) fail(key, "type");
    const std::string& s = v->get<std::string>();
    if (s.size() < min_len) fail(key, "minLength");
    if (s.size() > max_len) fail(key, "maxLength");
}

bool no_edge_whitespace(const std::string& s) {
    if (s.empty()) return false;
    auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    return !is_space(s.front()) && !is_space(s.back());
}

bool rfc3339(const std::string& s, double& epoch) {
    // Strict RFC 3339 date-time: full date, 'T' (or 't'), full time with seconds, optional fraction, offset required.
    if (s.size() < 20) return false;
    if (s[10] != 'T' && s[10] != 't') return false;
    auto parsed = parse_iso8601(s);
    if (!parsed || !parsed->has_offset) return false;
    // seconds must be present: HH:MM:SS
    if (s.size() < 19 || s[13] != ':' || s[16] != ':') return false;
    epoch = parsed->epoch_seconds;
    return true;
}

}  // namespace

OrderEvent parse_order_message(std::string_view raw) {
    if (raw.size() > kMaxOrderMessageBytes) throw std::invalid_argument("Order snapshot exceeds 64 KiB.");
    json payload;
    try {
        payload = json::parse(raw);
    } catch (const json::exception&) {
        throw std::invalid_argument("Order snapshot is not valid JSON.");
    }
    return parse_order_payload(payload);
}

OrderEvent parse_order_payload(const json& payload) {
    if (!payload.is_object()) fail("snapshot", "type");
    static const std::set<std::string> known = {
        "schema_version", "account_id", "account_num_id", "order_id", "sequence", "created_at", "updated_at", "symbol",
        "side", "order_type", "status", "quantity", "notional", "filled_quantity", "average_fill_price", "limit_price",
        "stop_price", "currency", "time_in_force", "command_id", "client_order_id", "message"};
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        if (!known.count(it.key())) fail("snapshot", "additionalProperties");
    }
    for (const char* key : {"schema_version", "account_id", "order_id", "sequence", "created_at", "updated_at", "symbol", "side",
                            "order_type", "status", "quantity", "filled_quantity", "average_fill_price"}) {
        if (!payload.contains(key)) fail("snapshot", "required");
    }
    const json& version = payload.at("schema_version");
    if (!is_integer_value(version)) fail("schema_version", "type");
    if (integer_value(version) != 1) fail("schema_version", "const");

    require_string(payload, "account_id", 1, 128, true);
    if (!no_edge_whitespace(payload.at("account_id").get<std::string>())) fail("account_id", "pattern");
    if (const json* num = json_find(payload, "account_num_id")) {
        if (!is_integer_value(*num)) fail("account_num_id", "type");
        if (integer_value(*num) < 1) fail("account_num_id", "minimum");
    }
    require_string(payload, "order_id", 1, 256, true);
    if (!no_edge_whitespace(payload.at("order_id").get<std::string>())) fail("order_id", "pattern");
    const json& sequence = payload.at("sequence");
    if (!is_integer_value(sequence)) fail("sequence", "type");
    if (integer_value(sequence) < 0) fail("sequence", "minimum");
    if (integer_value(sequence) > 9007199254740991LL) fail("sequence", "maximum");
    require_string(payload, "created_at", 0, 40, true);
    require_string(payload, "updated_at", 0, 40, true);
    double created = 0, updated = 0;
    if (!rfc3339(payload.at("created_at").get<std::string>(), created)) fail("created_at", "format");
    if (!rfc3339(payload.at("updated_at").get<std::string>(), updated)) fail("updated_at", "format");
    require_string(payload, "symbol", 1, 64, true);
    {
        const std::string& symbol = payload.at("symbol").get<std::string>();
        for (char c : symbol) if (std::isspace(static_cast<unsigned char>(c))) fail("symbol", "pattern");
    }
    static const std::set<std::string> sides = {"BUY", "SELL"};
    static const std::set<std::string> types = {"MARKET", "LIMIT", "STOP", "STOP_LIMIT", "TRAILING_STOP", "OTHER"};
    static const std::set<std::string> statuses = {"PENDING_NEW", "NEW", "PARTIALLY_FILLED", "PENDING_CANCEL", "FILLED",
                                                   "CANCELED", "REJECTED", "EXPIRED", "UNKNOWN"};
    const json& side = payload.at("side");
    if (!side.is_string() || !sides.count(side.get<std::string>())) fail("side", "enum");
    const json& order_type = payload.at("order_type");
    if (!order_type.is_string() || !types.count(order_type.get<std::string>())) fail("order_type", "enum");
    const json& status = payload.at("status");
    if (!status.is_string() || !statuses.count(status.get<std::string>())) fail("status", "enum");

    auto check_number = [&](const char* key, bool nullable, bool exclusive_min_zero, bool min_zero) {
        const json* v = json_find(payload, key);
        if (!v) return;
        if (v->is_null()) {
            if (!nullable) fail(key, "type");
            return;
        }
        if (!v->is_number() || v->is_boolean()) fail(key, "type");
        double d = v->get<double>();
        if (!std::isfinite(d)) throw std::invalid_argument("Numeric values must be finite.");
        if (exclusive_min_zero && d <= 0) fail(key, "exclusiveMinimum");
        if (min_zero && d < 0) fail(key, "minimum");
    };
    check_number("quantity", true, true, false);
    check_number("notional", false, true, false);
    check_number("filled_quantity", false, false, true);
    check_number("average_fill_price", true, false, true);
    check_number("limit_price", true, false, true);
    check_number("stop_price", true, false, true);
    if (const json* currency = json_find(payload, "currency")) {
        if (!currency->is_string()) fail("currency", "type");
        const std::string& c = currency->get<std::string>();
        bool ok = c.size() == 3;
        for (char ch : c) if (ch < 'A' || ch > 'Z') ok = false;
        if (!ok) fail("currency", "pattern");
    }
    require_string(payload, "time_in_force", 1, 32, false);
    require_string(payload, "command_id", 1, 256, false);
    require_string(payload, "client_order_id", 1, 256, false);
    require_string(payload, "message", 0, 2000, false);

    // Conditional requirements (allOf).
    if (payload.at("quantity").is_null() && !payload.contains("notional")) fail("snapshot", "required");
    std::string type = order_type.get<std::string>();
    if (type == "LIMIT" || type == "STOP_LIMIT") {
        const json* lp = json_find(payload, "limit_price");
        if (!lp) fail("snapshot", "required");
        if (!lp->is_number()) fail("limit_price", "type");
    }
    if (type == "STOP" || type == "STOP_LIMIT") {
        const json* sp = json_find(payload, "stop_price");
        if (!sp) fail("snapshot", "required");
        if (!sp->is_number()) fail("stop_price", "type");
    }

    // events.py cross-field rules.
    if (updated < created) throw std::invalid_argument("Timestamps require a UTC offset and updated_at >= created_at.");
    double filled = payload.at("filled_quantity").get<double>();
    std::optional<double> quantity;
    if (!payload.at("quantity").is_null()) quantity = payload.at("quantity").get<double>();
    if (quantity && filled > *quantity) throw std::invalid_argument("filled_quantity cannot exceed quantity.");
    std::string st = status.get<std::string>();
    if (st == "FILLED" && (filled <= 0 || (quantity && filled != *quantity))) {
        throw std::invalid_argument("FILLED requires the entire quantity to be filled.");
    }
    if (st == "PARTIALLY_FILLED" && (filled <= 0 || (quantity && filled >= *quantity))) {
        throw std::invalid_argument("PARTIALLY_FILLED requires a positive, incomplete fill.");
    }
    OrderEvent event;
    event.payload = payload;
    event.created_at = created;
    event.updated_at = updated;
    event.created_at_iso = iso_utc(created);
    event.updated_at_iso = iso_utc(updated);
    return event;
}

}  // namespace ng::smart
