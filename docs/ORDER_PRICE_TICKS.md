# Limit-order price ticks

The control server normalizes outgoing `LIMIT_ORDER` and `LIMIT_ORDER_FOK`
prices at the common publication boundary, before the Kafka command sink.
This covers manual/account-card limits, percentage-through-market limits,
fast-trading FOK commands, Smart limit commands and Posting orders.

USD equity limit prices below $1 use a $0.0001 tick. At or above $1 they use
the configured instrument tick, normally $0.01. The band is selected from each
proposed order price, so $0.0910 and $0.0911 remain distinct. Exact
halfway values round upward. With a `0.01` tick, `39.501` becomes `39.50` and
`39.505` becomes `39.51`. Zero, nonfinite, invalid, or prices that round to zero
are rejected before publication and external-order tracking.

The at-or-above-$1 tick comes from the existing instrument rules: `smart_selling` for SELL
and `smart_buying` for BUY, with per-symbol `instruments.<symbol>.tick_size`
overriding `instrument_defaults.tick_size`. The normal default is `0.01`;
configured smaller or non-cent ticks are honored in that upper band. Custom
`tick_ladder` configuration remains unsupported.

E/F round their execution price before allocating buying capacity, and round a
buying upper bound down or a selling lower bound up so the user limit is never
crossed. G/H apply the same price bands during planning, sell dispatch checks and
buying-target sizing. Existing parents with a persisted `tick_size` of `0.01`
also use the sub-dollar grid; their total targets are not increased by this fix.
P uses adjacent legal price levels for its spread test and ask-minus-one-tick
rule, including the $0.9999 / $1.0000 boundary, and keeps its minimum price.

Percentage-based prices use decimal arithmetic and go directly to tick rounding,
without an intermediate four-decimal rounding. Account-card responses include
the rounded `limit_price`, and an explicitly entered price is updated in the form.
Percentage-mode forms stay in percentage mode for the next request.

The change applies to new requests. Previously rejected commands are not replayed
or resubmitted. Broker rate limiting, cancellation confirmation and configurable
zero-default action delays are unchanged.
