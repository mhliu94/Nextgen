# Migrating from the Python services to `nextgen-gui` and `nextgen-orders`

`nextgen-gui` is a drop-in replacement for the `python3 -m uvicorn trading_ui.webserver:app`
process started by `start_trading_ui.sh`; `nextgen-orders` replaces the `order_ui.webserver`
process started by `start_order_ui.sh`. Nothing else in the KTrader deployment changes: the
Tiger/Futu listeners, the market-data producers, `admin_ui` and `configurator_ui` keep running
as they are. Both servers follow KTrader up to commit `fec87a3`.

## 1. Build on the target box

```bash
git clone <this repository> ~/nextgen-gui
cd ~/nextgen-gui
./scripts/bootstrap.sh
```

The script installs the handful of build prerequisites with the distribution's package
manager (Debian/Ubuntu, RHEL/Fedora, SUSE, Alpine, Arch are recognised), builds with CMake +
Ninja and runs the test-suite. If the box has no package manager access, build elsewhere with
the same glibc major version and copy `build/nextgen-gui` and `build/nextgen-orders` plus
`scripts/`, `config/` and `deploy/`; the binaries only need `libssl`, `libcrypto`, `libz` and
libc at runtime.

## 2. Copy the configuration and state

| From the Python checkout | To | Notes |
| --- | --- | --- |
| `trading_ui/sample/config.local.json` (or wherever `ACCOUNT_DASHBOARD_CONFIG` points) | `config/config.local.json` | Same format, including the `order_ui`, `smart_buying` and `reporting` sections. Fix relative paths if the file moves directories (they resolve against the config file). |
| `trading_accounts.json` | `config/trading_accounts.json` | Same format; still editable with `configurator_ui`. |
| `market_data/md_producer_config.json` | wherever `market_data.publisher_config` points | Optional; its `symbols` drive the Market Data page. The old `historical_prices.csv` is no longer used (previous closes come from the live quotes). |
| `certs/server.crt`, `certs/server.key` | `certs/` | Optional; the start scripts generate a self-signed pair when both are missing. |
| `logs/smart_selling.sqlite3`, `logs/smart_buying.sqlite3` (+ `.feedback.sqlite3`, `.lock`) | wherever `smart_selling.state_db` / `smart_buying.state_db` point | Recommended if a parent is mid-flight or records are unreconciled; the journal formats are identical. |
| `reports/activity.sqlite3` | wherever `reporting.db_path` points | Keeps algorithm-session and tag history, market-data samples and system snapshots. |

Check it:

```bash
./build/nextgen-gui --check-config --config config/config.local.json
./build/nextgen-orders --check-config --config config/config.local.json
```

## 3. Environment variables

`scripts/start_nextgen_gui.sh` accepts the same variables as `start_trading_ui.sh` and applies
them with the same precedence (configuration file first, environment fills gaps,
`UI_SSL_ENABLED` and `MARKET_INSIGHTS_MAX_LEVELS` override the file):

```
ACCOUNT_DASHBOARD_CONFIG   UI_HOST  UI_PORT  UI_SSL_ENABLED  UI_SSL_CERTFILE  UI_SSL_KEYFILE  UI_TLS_NAME
UI_REDIRECT_ENABLED  UI_REDIRECT_HOST  UI_REDIRECT_PORT  UI_REDIRECT_TARGET_HOST  UI_REDIRECT_TARGET_PORT
KAFKA_BOOTSTRAP_SERVERS  KAFKA_MARKET_DATA_BOOTSTRAP_SERVERS  KAFKA_ACCOUNT_DETAILS_TOPIC
KAFKA_TRADING_COMMANDS_TOPIC  KAFKA_MARKET_DATA_TOPIC  KAFKA_STOCK_QUOTES_TOPIC  KAFKA_GROUP_ID
KAFKA_MARKET_DATA_GROUP_ID  KAFKA_AUTO_OFFSET_RESET  KAFKA_MARKET_DATA_AUTO_OFFSET_RESET
KAFKA_MARKET_DATA_REPLAY_RECORDS  MARKET_INSIGHTS_MAX_LEVELS  OPERATIONS_LOG_DIR  LOG_LEVEL
```

`scripts/start_nextgen_orders.sh` accepts the variables of `start_order_ui.sh`:

```
ORDER_UI_CONFIG (or ACCOUNT_DASHBOARD_CONFIG)  ORDER_UI_HOST  ORDER_UI_PORT
ORDER_UI_SSL_ENABLED  ORDER_UI_SSL_CERTFILE  ORDER_UI_SSL_KEYFILE  LOG_LEVEL
```

The Python launcher's defaults bound the services to one public address; the new scripts
listen on `0.0.0.0` unless `UI_HOST` / `ORDER_UI_HOST` say otherwise. Put the variables in
`/etc/nextgen-gui.env` and `/etc/nextgen-orders.env` when running under systemd.

## 4. Switch over

1. Stop the Python services (with systemd, `systemctl stop <old units>`). Do this before
   starting the new control server if both share the Smart journals — a second owner is
   refused by the journal lock. The order monitor only reads; it can be switched at any time.
2. Start the new services:
   ```bash
   sudo cp deploy/nextgen-gui.service deploy/nextgen-orders.service /etc/systemd/system/
   sudo systemctl daemon-reload && sudo systemctl enable --now nextgen-gui nextgen-orders
   journalctl -u nextgen-gui -u nextgen-orders -f
   ```
3. Watch the feed pills in the control center's top bar (or `GET /api/health`):
   `account-details` and `market-data` go `live` as soon as the first messages arrive, the
   Smart `orders`/`volume` feeds go `replaying` → `live` after the retained order-status and
   trading-volume topics have been replayed. The order monitor shows "Loading retained order
   snapshots" until its replay reaches the end of every partition.
4. Log in with the same users and passwords. Sessions are in memory, as before, so everyone
   signs in again after a restart.

Rolling back is the reverse: stop the new services, start the Python ones. The journals and
the report database are shared, and the trading-commands topic has not changed format.

## 5. Kafka consumer groups

The control server uses the configured group ids (`kafka.group_id`,
`kafka.market_data_group_id`) exactly like the Python service, so it inherits committed
offsets for `account-details`. On assignment the market-data consumer replays only the last
`kafka.market_data_replay_records` (default 100) records of each partition, and the last 25
minutes of `stock-quotes` by timestamp, as upstream does now. The Smart feeds, the market-data
order feed and the order monitor replay their retained topics from the beginning with unique
group ids and never commit. If you run old and new servers side by side for comparison, give
the new one distinct group ids, distinct `state_db` files and a distinct `reporting.db_path`.

## 6. What changed for browsers and scripts

* All control-center pages are one client-side application. Deep links and the navigation are
  the same; the server no longer renders HTML tables, so bookmarks work but screen-scraping
  the old templates does not.
* `/api/account-details/stream` sends JSON events instead of HTML fragments. The multiplexed
  stream `/api/stream?topics=accounts,quotes,smart,algo,feeds,commands` is what the UI uses;
  see [API.md](API.md).
* Legacy form endpoints (`/submit-order`, `/submit-algo`, `/submit-broker-orders`, ...) still
  redirect back with `?ok=` / `?err=` and the UI shows those as toasts.
* Upstream changes carried over: Admin Control replaced Currency Conversion
  (`/currency-conversion`, `/submit-currency-conversion` and `/api/fx` are gone),
  `/api/market-data` became the `lmh`-only five-minute window API and the old quotes rows moved
  to `/api/quotes`.
* The `lang` cookie is shared by both UIs and `?lang=` links work in both. Logins and
  entitlements are unchanged: the users in `auth.users` (`admin` and `trader` mandatory) can
  sign in, and only `lmh` sees Market Data and Admin Control and may change trading status.

## 7. Persistence

The daily order files (`~/Reports/orders-<date>/orders-<date>.json` and
`source-snapshots.json`) are produced by the control server itself at the configured local
time, or on demand with `nextgen-gui --persist-orders [--date YYYY-MM-DD]`; point
`persistence.order_archive.dir` (or `ORDER_ARCHIVE_DIR`) at the directory the old script used.
Account holdings are recorded every minute under `persistence.account_snapshots.dir`
(`ACCOUNT_SNAPSHOTS_DIR`), as JSON lines per day plus a SQLite database, and — as upstream —
in the `system_snapshots` table of the report database.

## 8. Not migrated (by design)

`admin_ui/`, `configurator_ui/`, `trading_server/` (Tiger and Futu listeners), the
market-data producers, and `retrieve-all-orders.py` (a Tiger Open API SDK script that exports
a broker's order history to CSV and e-mails it; it talks to the broker directly and is not
part of either web service). They need no change.
