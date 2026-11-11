# Smart buying (mode H)

Mode H buys a fixed share of newly observed trading volume: **Patient 5 %, Normal 8 %,
Urgent 15 %**. The rates are fixed; a request cannot override them. Like the other algorithm
modes, its code lives apart from the plumbing, in `src/algo/smart_buying/`, on top of the
Smart selling engine it shares its durable ledger with.

## Starting a session

In the control panel choose accounts, symbol, urgency, a start time, an optional deadline and
an optional maximum buy price (or call `POST /api/smart-buying`). Each account's target is
its available USD cash (else total USD cash, else the legacy cash field) divided by the
current best ask rounded up to the price tick, rounded down to the quantity step. Targets are
fixed at creation; accounts that cannot afford the minimum are skipped. Starting requires
live order-status and trading-volume feeds, a fresh ask, enabled accounts with fresh
snapshots, and no unresolved orders on those accounts for that symbol.

## Execution

* Every child is at least **100 shares** (or a larger configured minimum). The controller
  waits until enough participation credit has been earned; it never rounds an unfunded slice
  up, and target remainders below the minimum are left unfilled.
* Orders are limit orders at the best ask, never above the maximum price. Size is capped by
  earned credit, displayed ask liquidity, the remaining target, the account's cash budget
  less confirmed spending, and its latest available cash. The bounds are rechecked right
  before publication, and confirmed fills consume cash at the limit price (a conservative
  upper bound) so a delayed account snapshot cannot spend the same cash twice.
* One parent rotates across its accounts with at most one unresolved child at a time,
  submissions at least five seconds apart, and a cancel request two seconds after a child
  starts working. Only terminal broker evidence releases a reservation.
* Buying pauses while at least five distinct displayed bid prices lie between the best offer
  and the maximum price (the same five-level rule Smart selling applies to its minimum
  price), and waits for depth when a non-marketable maximum lies beyond the book.
* A deadline stops execution; it does not raise participation. Stopping cancels working
  children and keeps the accounts until reconciliation completes.

Each account may run at most one buyer (E or H), alongside any number of sellers (F or G),
all on distinct symbols. Scheduled, paused, stopping and reconciling sessions retain their
reservations. Manual orders remain blocked on any account with an algorithm session.
Children are ordinary `LIMIT_ORDER` commands with
`side: BUY` and `trading_mode: H`, so brokers need no change.

## Configuration and API

```json
"smart_buying": {
  "enabled": true,
  "state_db": "../logs/smart_buying.sqlite3",
  "order_status_topic": "order-status",
  "trading_volume_topic": "trading-volume",
  "quote_max_age": 10, "trade_max_age": 15, "account_max_age": 60,
  "instrument_defaults": {"tick_size": "0.01", "quantity_step": "1", "min_quantity": "100", "min_notional": "0"},
  "instruments": {}
}
```

The state database must differ from `smart_selling.state_db`. Routes:
`GET/POST /api/smart-buying`, `GET /api/smart-buying/{id}/events`,
`POST /api/smart-buying/{id}/stop`; `POST /api/algo/stop` stops H parents too. The panel shows
sessions started today plus every active session.

## Verification

`build/nextgen-tests -sf="*test_smart_buying*"` and `-sf="*test_smart_order_limits*"` port the
Python suites, and `tools/algo_oracle` replays random scenarios through this engine and the
Python `SmartBuyingEngine` (`"engine": "buy"`) step by step with zero divergences.
