#pragma once
#include <stdexcept>
#include "core/decimal.h"

namespace ng {

// The persisted/configured tick is the at-or-above-$1 grid. Select the
// sub-dollar grid from each proposed price, including for recovered parents.
inline Decimal equity_tick_size(const Decimal& price, const Decimal& configured_tick) {
    if (!price.is_positive() || !configured_tick.is_positive())
        throw std::invalid_argument("Price and tick size must be positive");
    return price < Decimal(1) ? Decimal::parse("0.0001") : configured_tick;
}

inline Decimal round_equity_price_up(const Decimal& price, const Decimal& configured_tick) {
    Decimal rounded = price.round_up_to_step(equity_tick_size(price, configured_tick));
    // Crossing $1 must land on the upper band's grid as well.
    if (price < Decimal(1) && rounded >= Decimal(1)) rounded = rounded.round_up_to_step(configured_tick);
    return rounded;
}

inline Decimal round_equity_price_down(const Decimal& price, const Decimal& configured_tick) {
    Decimal rounded = price.round_down_to_step(equity_tick_size(price, configured_tick));
    if (price >= Decimal(1) && rounded < Decimal(1)) return Decimal::parse("0.9999");
    return rounded;
}

inline Decimal nearest_equity_price(const Decimal& price, const Decimal& configured_tick) {
    Decimal lower = round_equity_price_down(price, configured_tick);
    Decimal upper = round_equity_price_up(price, configured_tick);
    return price - lower < upper - price ? lower : upper; // halfway upward
}

inline Decimal previous_equity_price(const Decimal& price, const Decimal& configured_tick) {
    Decimal lower = round_equity_price_down(price, configured_tick);
    if (lower < price) return lower;
    lower = price - equity_tick_size(price, configured_tick);
    // $0.9999 is the immediate predecessor of $1, not $0.99.
    if (price >= Decimal(1) && lower < Decimal(1)) return Decimal::parse("0.9999");
    return lower;
}

} // namespace ng
