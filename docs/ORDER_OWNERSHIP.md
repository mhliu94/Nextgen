# Order ownership and cancellation

The control server owns the distinction between algorithm and manual orders. It
persists algorithm command/session mappings and UI manual command mappings before
publishing submissions. Broker-app orders are external manual orders when no
unresolved algorithm submission could explain them. Ambiguous ownership is shown
separately and cannot be cancelled through manual controls. Exact command identity
survives later status-only snapshots and process restarts.

`/api/working-orders` supplies the admin application's Working orders tab. It uses
the durable activity order projection, including prior-day GTC orders. Only
nonterminal orders appear. Algorithm rows show the stable public session ID.
Browser payload privacy filtering still applies.

`POST /api/orders/cancel` accepts `account_ids`, optional `symbol`, and optional
`order_id` for an individual order on exactly one account. `__ALL__` selects every
visible configured account. The server selects confirmed manual orders and emits
one `CANCEL_ORDER` per exact broker ID. It never emits a broad broker cancel-all.
Caller-supplied ownership is ignored. An algorithm owning the account does not
prevent cancellation of existing manual orders; it still prevents new manual UI
submissions. Broker-app submission itself is outside the server's control.

The independent order feed must finish replay before manual cancellations are
accepted. Batch results distinguish queued, already pending, excluded algorithm,
unresolved, and publication-error orders. A durable cancellation claim prevents
HTTP retries or restarts from duplicating a request. A queued/uncertain request
remains visible until terminal broker feedback; an acknowledgment is not a fill or
a confirmed cancellation. Uncertain requests need reconciliation rather than a
new cancellation ID.

G/H cancellation intents use the current child's exact broker ID, including stop,
deadline, and other engine cancellation reasons. They cannot sweep up manual
orders. E/F stop cleanup scans only orders correlated to that session. It also
handles broker acknowledgments arriving after the session ends. Ended-session
cleanup continues on the control server's status loop after reconnection/replay.
Other sessions' orders and manual orders are excluded.

All E/F/G/H starts reject the entire request if any selected account has a missing,
invalid, future-dated, or older-than-five-minutes account snapshot. The checklist
marks those accounts offline. Snapshot eligibility is rechecked during algorithm
operation and before publication. G/H additionally retain their stricter configured
account-age checks. Account ownership remains reserved against manual UI entry
while confirmed algorithm orders remain open.

The Algo / 算法 tab sits beside Trading and is available to every authenticated
user. It lists only active sessions across E/F/G/H, including paused and stopping
sessions; concluded sessions stay in the order UI reports. Individual stops target
the durable session ID (including E/F sessions sharing a ticker), and are available
to every authenticated user. Only the exact username `lmh` may stop all sessions;
both the API and legacy form enforce that restriction. Initial sell inventory and
per-account initial quantities/costs are omitted from the active-session display.

Broker rollout must precede activating this control-server build. Bosco,
GigaMoney, Fangde and Star need the exact-cancellation dispatcher changes;
Zhuorui needs strict exact-ID parsing and creation-day history lookup for older
orders. Bitget already supports exact-ID cancellation. Use an operator-authorized
restart window. BOSCO-WJY stays stopped until the operator explicitly requests it.
