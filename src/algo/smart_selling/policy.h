// Pure arithmetic and fixed execution policy for Smart selling (smart_selling_policy.py).
// No clocks, no I/O: the engine supplies the decision time and quote snapshot.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/decimal.h"
#include "core/json.h"

namespace ng::smart {

// Smart selling percentage units: patient 6%, normal 12%, urgent 20%, ultra 30%.
const std::map<std::string, Decimal>& participation_percentages();
// Mode names in their declaration order (error messages list them like Python's dict keys).
const std::vector<std::string>& participation_modes();
// Smart buying (mode H): patient 5%, normal 8%, urgent 15%.
const std::map<std::string, Decimal>& buying_participation_percentages();
const std::vector<std::string>& buying_participation_modes();
// Every child needs at least 100 shares, or the larger configured broker minimum.
const Decimal& min_order_quantity();

struct Defaults {
    static constexpr double horizon_seconds = 7200;
    static constexpr const char* mode = "normal";
    static constexpr double quote_max_age = 5;
    static constexpr double trade_max_age = 10;
    static constexpr const char* time_in_force = "DAY";
};

// Decimal(str(value)) with Python semantics: bools, NaN/Infinity and junk are rejected.
Decimal parse_decimal(const json& value, const std::string& label = "quantity", bool positive = false,
                      bool nonnegative = false);
Decimal parse_decimal(const std::string& text, const std::string& label = "quantity", bool positive = false,
                      bool nonnegative = false);
// Bounded external precision: at most 30 digits, adjusted exponent <= 24, exponent >= -24.
Decimal parse_input_decimal(const json& value, const std::string& label = "quantity", bool positive = false,
                            bool nonnegative = false);
Decimal parse_participation_percentage(const json& value, const std::string& label = "participation percentage");
// Exact plain string for JSON/SQLite payloads.
std::string decimal_string(const Decimal& value);
// Epoch seconds from epoch numbers/numeric strings or ISO datetimes with an offset; nullopt for null.
std::optional<double> parse_timestamp(const json& value);
std::optional<std::string> utc_timestamp_string(const std::optional<double>& value);

bool meets_order_minimum(const json& parent, const Decimal& quantity, const Decimal& price);

struct QuoteState {
    Decimal bid;
    Decimal ask;
    Decimal bid_size;
    Decimal ask_size;
    // Distinct displayed prices with positive size: bids descending, asks ascending.
    std::vector<Decimal> bid_prices;
    std::vector<Decimal> ask_prices;
    Decimal consumed;
    std::optional<double> consumed_wall;
    double received_at = 0.0;
    std::optional<double> source_time;
    std::optional<long long> sequence;
    bool healthy = false;
    bool accessible = true;
    bool sequence_gap = false;
    long long version = 0;
};

struct ChildPlan {
    std::optional<Decimal> quantity;  // nullopt: wait without sending
    Decimal limit_price;
    bool aggressive = false;
    bool depth_limited = false;
    std::string reason;
};

ChildPlan plan_child_order(const json& parent, const QuoteState& quote, double now);
// Smart buying: a best-ask limit within the optional ceiling, sized by earned credit, displayed ask
// liquidity and the remaining target (smart_buying.plan_buy).
ChildPlan plan_buy(const json& parent, const QuoteState& quote, double now);
// Counts distinct displayed prices from the executable side toward the user's price bound
// (minimum_price when selling, maximum_price when buying). Returns "" when trading may proceed, or
// the reason to pause: the bound is five or more displayed levels away, or beyond the displayed depth.
std::string price_distance_reason(const json& parent, const QuoteState& quote, const std::string& side);

}  // namespace ng::smart
