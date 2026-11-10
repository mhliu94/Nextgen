# Reconciling Smart selling / Smart buying records

Smart selling (G) and Smart buying (H) never guess an order's fate. An account stays locked
for a symbol while the controller knows of a broker order without a final status, a command
that no final report has resolved yet, or a dispatched child of a stopped session. The order
monitor's **Unreconciled orders** tab lists exactly these records
([ORDER_MONITOR.md](ORDER_MONITOR.md)). Normally they clear themselves when the broker
publishes the final `order-status` snapshot. Two operator utilities, ports of KTrader's
`trading_ui.reconcile_prior_day` and `trading_ui.reconcile_smart_selling`, handle the cases
that do not.

Both are subcommands of the control-server binary. Both **preview by default** and change
nothing until `--apply` is given; neither publishes commands, cancels orders, restarts
services or resumes a session. Check the broker's actual order status before applying.

## Prior-day leftovers: `reconcile-prior-day`

Broker orders and commands from earlier trading days that never received a final report (for
example because a listener was down) can be marked reconciled in one step:

```bash
build/nextgen-gui reconcile-prior-day --config config/config.local.json \
    --reason "Operator requested reconciliation of records before the current trading day"

# after reviewing the preview:
build/nextgen-gui reconcile-prior-day --config config/config.local.json \
    --reason "..." --apply --backup-dir backups/reconcile-2026-11-10
```

* The trading day is today's date in `order_ui.timezone` (default `America/New_York`).
* Sources are the configured `smart_selling.state_db` and `smart_buying.state_db` with their
  `.feedback.sqlite3` stores; all must exist and be distinct files.
* Only non-terminal broker orders **created** before the trading day, and pending commands
  whose id embeds a timestamp before the trading day, are selected. Commands without an
  embedded timestamp are listed separately and left alone. Anything that belongs to an active
  algorithm child is refused ("reconcile that child first").
* `--apply` requires `--backup-dir`, which must not exist yet: every database is backed up
  there (SQLite online backup), the preview is written to `preview.json`, the changes are made
  in one transaction per store with an audit row carrying your reason, and the outcome goes
  to `result.json`. Reconciled orders are moved to `feedback_reconciled_orders` with their
  previous data, so newer broker evidence can still reopen them.

## A stopped session's child: `reconcile-stopped-child`

If a broker listener omitted both `command_id` and `client_order_id`, even a filled child can
leave a stopped session waiting. After reviewing which broker order the child became, save
that order's final `order-status` snapshot (the latest one the feedback store holds) as JSON
and preview the repair:

```bash
build/nextgen-gui reconcile-stopped-child \
    --state-db logs/smart_selling.sqlite3 \
    --feedback-db logs/smart_selling.sqlite3.feedback.sqlite3 \
    --client-order-id smart-REVIEWED-CLIENT-ID \
    --snapshot-file /path/to/terminal-snapshot.json \
    --source "Reviewed Kafka order-status partition and offset, plus command evidence"
```

Back up both databases, then add `--apply`. The utility requires a stopped session with a
dispatched outstanding child, a final broker status, matching account and order attributes, a
broker order no older than the child's submission and not bound to another child, and a
snapshot equal to the latest durable broker feedback. It runs the controller's normal
transition for the final report, records the evidence in `smart_events`
(`manual_terminal_reconciliation`) and releases the session atomically; the running server
picks the repaired state up on its next status check. Unlike the Python utility, the C++ port
also accepts Smart buying state databases.

Fix the publisher too: it must echo `command_id` or `client_order_id` so future orders are
attributed automatically.

## Tests

`build/nextgen-tests -sf="*test_reconcile*"` exercises both utilities on real SQLite stores:
previews, backups, refusal rules, apply results and idempotence.
