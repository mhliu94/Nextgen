# Finland deployment and account streams

Active broker workers publish account changes and order feedback to the Finland
Kafka external listener using SASL_SSL, SCRAM-SHA-512, and its trusted CA.
Nextgen on that host uses the loopback listener. Authentication files stay on the
servers and are never committed.

The account consumer accepts version 1 `account_snapshot` / `account_delta`
messages. Deltas replace changed values and explicitly remove positions or
currency entries. Sequence gaps disable the account until a full checkpoint;
duplicate events and retired streams cannot replace current state.

`tools/telemetry_bridge.py` preserves upstream market data and retained order
feedback during migration. It never copies trading commands. Its account allowlist
contains active, non-hidden accounts. Source offsets are committed only after
destination acknowledgements. Singapore Kafka remains the upstream market-data
source; its KTrader web/control processes are stopped.

## Browser privacy and session labels

Participation settings remain in private configuration and calculation state.
Both HTTP servers remove participation fields recursively at the browser boundary,
including cached JSON, event streams, reports, and serialized JSON documents.
The admin UI no longer renders a participation-rate label or value.

Session labels use `TICKER-MODE-SEQUENCE`, for example `POM-E-11`. The shared
`activity_session_ids` SQLite table allocates one AUTOINCREMENT sequence across
E/F/G/H. Existing sessions are numbered in start-time order. Duplicate start
notifications and restarts do not consume new numbers. Internal references
remain stable for order correlation. Both UIs display the public label.

## Holdings persistence

Deployed `persistence.account_snapshots` settings enable JSONL and SQLite
recording every 60 seconds under `/var/lib/nextgen/account-snapshots`.
`holdings.sqlite3` contains snapshot headers, account states, positions, and
currency balances. JSONL files contain complete account documents. `/api/health`
reports the last successful write and any persistence error.

## Verification and rollback

Verification includes C++ integration tests with mock Kafka, browser checks for
confirmations and reports on desktop/mobile, live authenticated read APIs,
SSE privacy, all active account feeds, preserved reports, and consecutive persisted
minute snapshots. Financial workflows use synthetic brokers in tests; deployment
verification does not place live orders.

Worker source/config backups are under `/var/lib/ktrader-finland-migration`.
Finland's prior binaries, configuration, and SQLite snapshots are under
`/var/lib/ktrader-finland-cutover-20261111`. A full rollback must coordinate broker
Kafka routes with the control server. Starting the old Singapore UI alone would
leave it on the old command cluster.
