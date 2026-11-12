# Limit-order price ticks

The control server normalizes outgoing `LIMIT_ORDER` and `LIMIT_ORDER_FOK`
prices at the common publication boundary, before the Kafka command sink.
This covers manual/account-card limits, percentage-through-market limits,
fast-trading FOK commands, and Smart limit commands.

Prices round to the nearest configured tick using decimal arithmetic. Exact
halfway values round upward. With a `0.01` tick, `39.501` becomes `39.50` and
`39.505` becomes `39.51`. Zero, nonfinite, invalid, or prices that round to zero
are rejected before publication and external-order tracking.

The tick comes from the existing instrument rules: `smart_selling` for SELL
and `smart_buying` for BUY, with per-symbol `instruments.<symbol>.tick_size`
overriding `instrument_defaults.tick_size`. The normal default is `0.01`;
configured smaller or non-cent ticks are honored. These are configured rules,
not automatic discovery of exchange price bands.

Percentage-based prices use decimal arithmetic and go directly to tick rounding,
without an intermediate four-decimal rounding. Account-card responses include
the rounded `limit_price`, and an explicitly entered price is updated in the form.
Percentage-mode forms stay in percentage mode for the next request.

The change applies to new requests. Previously rejected commands are not replayed
or resubmitted. Broker rate limiting, cancellation confirmation and configurable
zero-default action delays are unchanged.
