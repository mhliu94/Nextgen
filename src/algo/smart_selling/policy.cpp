#include "algo/smart_selling/policy.h"

#include <cmath>
#include <stdexcept>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng::smart {

const std::map<std::string, Decimal>& participation_percentages() {
    static const std::map<std::string, Decimal> table = {
        {"patient", Decimal::parse("3")}, {"normal", Decimal::parse("6")}, {"urgent", Decimal::parse("10")}};
    return table;
}

namespace {

std::string python_str(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_float()) return format_double(value.get<double>());
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
    Decimal min_quantity = parse_decimal(parent.at("min_quantity"));
    Decimal min_notional = parse_decimal(parent.at("min_notional"));
    return quantity >= min_quantity && quantity * price >= min_notional;
}

ChildPlan plan_child_order(const json& parent, const QuoteState& quote, double now) {
    Decimal parent_quantity = parse_decimal(parent.at("quantity"));
    Decimal filled = parse_decimal(parent.at("F"));
    Decimal reserved = parse_decimal(parent.at("R"));
    Decimal credit = parse_decimal(parent.at("C"));
    Decimal child_cap = parse_decimal(parent.at("child_cap"));
    Decimal quantity_step = parse_decimal(parent.at("quantity_step"));
    Decimal remaining = parent_quantity - filled - reserved;
    Decimal depth_budget = Decimal::max(Decimal::zero(), quote.bid_size - quote.consumed) * Decimal::parse(".10");
    bool depth_limited = depth_budget < Decimal::min(child_cap, remaining);
    Decimal ready_quantity = Decimal::min(Decimal::min(child_cap, remaining), depth_budget).round_down_to_step(quantity_step);

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
    bool aggressive = mode == "urgent" || fallback || behind_schedule || imminent;
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
        return ChildPlan{std::nullopt, price, aggressive, depth_limited, "depth or residual quantity below broker minimum"};
    }
    const json& last_send = parent.at("last_send");
    double flush_since = last_send.is_null() ? parent.at("start_mono").get<double>() : last_send.get<double>();
    if (credit < ready_quantity && now - flush_since < 30) {
        return ChildPlan{std::nullopt, price, aggressive, depth_limited, "earning volume credit"};
    }
    Decimal quantity = Decimal::min(ready_quantity, Decimal::max(Decimal::zero(), credit)).round_down_to_step(quantity_step);
    if (!meets_order_minimum(parent, quantity, price)) {
        return ChildPlan{std::nullopt, price, aggressive, depth_limited, "earned credit below broker minimum"};
    }
    return ChildPlan{quantity, price, aggressive, depth_limited, ""};
}

}  // namespace ng::smart
