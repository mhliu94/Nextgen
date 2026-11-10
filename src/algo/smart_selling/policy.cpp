#include "algo/smart_selling/policy.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng::smart {

const std::map<std::string, Decimal>& participation_percentages() {
    static const std::map<std::string, Decimal> table = {{"patient", Decimal::parse("6")},
                                                         {"normal", Decimal::parse("12")},
                                                         {"urgent", Decimal::parse("20")},
                                                         {"ultra", Decimal::parse("30")}};
    return table;
}

const std::vector<std::string>& participation_modes() {
    static const std::vector<std::string> modes = {"patient", "normal", "urgent", "ultra"};
    return modes;
}

const std::map<std::string, Decimal>& buying_participation_percentages() {
    static const std::map<std::string, Decimal> table = {
        {"patient", Decimal::parse("5")}, {"normal", Decimal::parse("8")}, {"urgent", Decimal::parse("15")}};
    return table;
}

const std::vector<std::string>& buying_participation_modes() {
    static const std::vector<std::string> modes = {"patient", "normal", "urgent"};
    return modes;
}

const Decimal& min_order_quantity() {
    static const Decimal minimum(100);
    return minimum;
}

namespace {

std::string python_str(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_float()) return python_float_repr(value.get<double>());
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_null()) return "None";
    return value.dump();
}

}  // namespace

Decimal parse_decimal(const std::string& text, const std::string& label, bool positive, bool nonnegative) {
    auto value = Decimal::try_parse(text);
    if (!value) throw std::invalid_argument(label + " must be a finite number");
    if ((positive && *value <= Decimal::zero()) || (nonnegative && value->is_negative())) {
        throw std::invalid_argument("invalid " + label);
    }
    return *value;
}

Decimal parse_decimal(const json& value, const std::string& label, bool positive, bool nonnegative) {
    if (value.is_boolean()) throw std::invalid_argument(label + " must be a finite number");
    if (value.is_null() || value.is_object() || value.is_array()) {
        throw std::invalid_argument(label + " must be a finite number");
    }
    return parse_decimal(python_str(value), label, positive, nonnegative);
}

Decimal parse_input_decimal(const json& value, const std::string& label, bool positive, bool nonnegative) {
    Decimal number = parse_decimal(value, label, positive, nonnegative);
    if (number.digit_count() > 30 || number.adjusted() > 24 || number.exponent() < -24) {
        throw std::invalid_argument(label + " exceeds supported decimal precision");
    }
    return number;
}

Decimal parse_participation_percentage(const json& value, const std::string& label) {
    Decimal percentage = parse_input_decimal(value, label, true, false);
    if (percentage > Decimal(100)) throw std::invalid_argument(label + " must be greater than 0 and at most 100");
    return percentage;
}

std::string decimal_string(const Decimal& value) { return value.str(); }

std::optional<double> parse_timestamp(const json& value) {
    if (value.is_null()) return std::nullopt;
    if (value.is_boolean()) throw std::invalid_argument("timestamp must be a finite number");
    if (value.is_number()) {
        double d = value.get<double>();
        if (!std::isfinite(d)) throw std::invalid_argument("invalid timestamp");
        Decimal check = parse_input_decimal(value, "timestamp");
        (void)check;
        return d;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        // Numeric strings (digits with an optional single '.') are epoch seconds.
        std::string digits_only = text;
        std::size_t dot = digits_only.find('.');
        if (dot != std::string::npos) digits_only.erase(dot, 1);
        bool numeric = !digits_only.empty();
        for (char c : digits_only) if (!std::isdigit(static_cast<unsigned char>(c))) numeric = false;
        if (numeric) return parse_input_decimal(value, "timestamp").to_double();
        auto parsed = parse_iso8601(text);
        if (!parsed) throw std::invalid_argument("invalid timestamp");
        if (!parsed->has_offset) throw std::invalid_argument("timestamps must include a UTC offset");
        return parsed->epoch_seconds;
    }
    throw std::invalid_argument("invalid timestamp");
}

std::optional<std::string> utc_timestamp_string(const std::optional<double>& value) {
    if (!value) return std::nullopt;
    return iso_utc(*value);
}

bool meets_order_minimum(const json& parent, const Decimal& quantity, const Decimal& price) {
    // Both quantity and notional rules must hold after downward sizing.
    Decimal min_quantity = Decimal::max(min_order_quantity(), parse_decimal(parent.at("min_quantity")));
    Decimal min_notional = parse_decimal(parent.at("min_notional"));
    return quantity >= min_quantity && quantity * price >= min_notional;
}

std::string price_distance_reason(const json& parent, const QuoteState& quote, const std::string& side) {
    bool selling = side == "SELL";
    const json* raw_bound = json_find(parent, selling ? "minimum_price" : "maximum_price");
    if (!raw_bound || raw_bound->is_null()) return "";
    Decimal bound = parse_decimal(*raw_bound);
    const Decimal& best = selling ? quote.bid : quote.ask;
    if ((selling && bound <= best) || (!selling && bound >= best)) return "";
    // Displayed prices beyond the best executable price, ordered toward the bound.
    std::vector<Decimal> prices;
    for (const Decimal& price : selling ? quote.ask_prices : quote.bid_prices) {
        if (selling ? price > best : price < best) prices.push_back(price);
    }
    std::sort(prices.begin(), prices.end(), [&](const Decimal& a, const Decimal& b) { return selling ? a < b : a > b; });
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
    int crossed = 0;
    for (const Decimal& price : prices) {
        if (selling ? price <= bound : price >= bound) ++crossed;
    }
    if (crossed >= 5) return "user price limit is at least five displayed levels away";
    if (prices.empty() || (selling ? bound > prices.back() : bound < prices.back())) {
        return "waiting for displayed depth to locate user price limit";
    }
    return "";
}

ChildPlan plan_child_order(const json& parent, const QuoteState& quote, double now) {
    Decimal parent_quantity = parse_decimal(parent.at("quantity"));
    Decimal filled = parse_decimal(parent.at("F"));
    Decimal reserved = parse_decimal(parent.at("R"));
    Decimal credit = parse_decimal(parent.at("C"));
    Decimal child_cap = parse_decimal(parent.at("child_cap"));
    Decimal quantity_step = parse_decimal(parent.at("quantity_step"));
    Decimal remaining = parent_quantity - filled - reserved;
    // The 10% displayed-size rule is not enforced for Smart selling.
    bool disable_child_cap = parent.value("disable_child_cap", false);
    Decimal size_cap = disable_child_cap ? remaining : child_cap;
    Decimal continuation = parent.contains("slice_remaining") ? parse_decimal(parent.at("slice_remaining")) : Decimal::zero();
    if (!continuation.is_zero()) size_cap = Decimal::min(size_cap, continuation);
    bool depth_limited = false;
    Decimal ready_quantity = Decimal::min(size_cap, remaining).round_down_to_step(quantity_step);

    bool behind_schedule = false;
    const json& deadline_mono = parent.at("deadline_mono");
    if (!deadline_mono.is_null()) {
        double start_mono = parent.at("start_mono").get<double>();
        double horizon = deadline_mono.get<double>() - start_mono;
        double elapsed_fraction = horizon > 0 ? std::max(0.0, std::min(1.0, (now - start_mono) / horizon)) : 1.0;
        Decimal progress_reference = parent_quantity * Decimal::from_double(elapsed_fraction);
        behind_schedule = filled < progress_reference - child_cap;
    }
    std::string mode = parent.at("mode").get<std::string>();
    bool fallback = parent.value("fallback", false);
    bool imminent = parent.value("imminent_event", false);
    bool aggressive = mode == "urgent" || mode == "ultra" || fallback || behind_schedule || imminent;
    Decimal selected_price;
    if (aggressive) selected_price = quote.bid;
    else if (mode == "patient") selected_price = quote.ask;
    else selected_price = (quote.bid + quote.ask).div(Decimal(2), 30);
    if (selected_price <= quote.bid) {
        aggressive = true;
        selected_price = quote.bid;
    }
    Decimal minimum_price;
    const json& min_price = parent.at("minimum_price");
    if (!min_price.is_null() && !(min_price.is_string() && min_price.get<std::string>().empty())) {
        minimum_price = parse_decimal(min_price);
    }
    Decimal price = Decimal::max(Decimal::max(selected_price, quote.bid), minimum_price)
                        .round_up_to_step(parse_decimal(parent.at("tick_size")));

    if (!meets_order_minimum(parent, ready_quantity, price)) {
        return ChildPlan{std::nullopt, price, aggressive, depth_limited, "residual quantity below broker minimum"};
    }
    const json& last_send = parent.at("last_send");
    double flush_since = last_send.is_null() ? parent.at("start_mono").get<double>() : last_send.get<double>();
    if (!disable_child_cap && credit < ready_quantity && now - flush_since < 30) {
        return ChildPlan{std::nullopt, price, aggressive, depth_limited, "earning volume credit"};
    }
    Decimal quantity = Decimal::min(ready_quantity, Decimal::max(Decimal::zero(), credit)).round_down_to_step(quantity_step);
    if (!meets_order_minimum(parent, quantity, price)) {
        return ChildPlan{std::nullopt, price, aggressive, depth_limited, "earned credit below broker minimum"};
    }
    return ChildPlan{quantity, price, aggressive, depth_limited, ""};
}

ChildPlan plan_buy(const json& parent, const QuoteState& quote, double now) {
    (void)now;
    Decimal price = quote.ask.round_up_to_step(parse_decimal(parent.at("tick_size")));
    const json& ceiling = parent.at("maximum_price");
    if (!ceiling.is_null() && price > parse_decimal(ceiling)) {
        return ChildPlan{std::nullopt, price, true, false, "best ask exceeds maximum price"};
    }
    Decimal remaining = parse_decimal(parent.at("quantity")) - parse_decimal(parent.at("F")) - parse_decimal(parent.at("R"));
    Decimal depth = Decimal::max(Decimal::zero(), quote.ask_size - quote.consumed);
    Decimal credit = Decimal::max(Decimal::zero(), parse_decimal(parent.at("C")));
    Decimal quantity = Decimal::min(Decimal::min(remaining, depth), credit).round_down_to_step(parse_decimal(parent.at("quantity_step")));
    if (!meets_order_minimum(parent, quantity, price)) {
        return ChildPlan{std::nullopt, price, true, depth < Decimal(100),
                         "waiting for volume credit and at least 100 shares of liquidity"};
    }
    return ChildPlan{quantity, price, true, depth < remaining, ""};
}

}  // namespace ng::smart
