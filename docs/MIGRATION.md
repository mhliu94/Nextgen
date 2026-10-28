# Migrating from `trading_ui` (Python) to `nextgen-gui`

The server is a drop-in replacement for the `python3 -m uvicorn trading_ui.webserver:app`
process started by `start_trading_ui.sh`. Nothing else in the KTrader deployment changes:
the Tiger/Futu listeners, the market-data producers, `order_ui`, `admin_ui` and
`configurator_ui` keep running as they are.

## 1. Build on the target box

```bash
git clone <this repository> ~/nextgen-gui
cd ~/nextgen-gui
./scripts/bootstrap.sh
```

The script installs the handful of build prerequisites with the distribution's package
manager (Debian/Ubuntu, RHEL/Fedora, SUSE, Alpine, Arch are recognised), builds with CMake +
Ninja and runs the test-suite. If the box has no package manager access, build elsewhere with
the same glibc major version and copy `build/nextgen-gui` plus `scripts/`, `config/` and
`deploy/`; the binary only needs `libssl`, `libcrypto`, `libz` and libc at runtime.

## 2. Copy the configuration

| From the Python checkout | To | Notes |
| --- | --- | --- |
| `trading_ui/sample/config.local.json` (or wherever `ACCOUNT_DASHBOARD_CONFIG` points) | `config/config.local.json` | Same format. Fix relative paths if the file moves directories (they resolve against the config file). |
| `trading_accounts.json` | `config/trading_accounts.json` | Same format; still editable with `configurator_ui`. |
| `market_data/historical_prices.csv` | `config/historical_prices.csv` | Or point `market_data.historical_prices_csv` at the existing file. |
| `certs/server.crt`, `certs/server.key` | `certs/` | Optional; the start script generates a self-signed pair when missing. |
| `logs/smart_selling.sqlite3` (+ `.feedback.sqlite3`, `.lock`) | wherever `smart_selling.state_db` points | Optional but recommended if a Smart selling parent is mid-flight; the journal format is identical. |

Check it:

```bash
./build/nextgen-gui --check-config --config config/config.local.json
```

## 3. Environment variables

`scripts/start_nextgen_gui.sh` accepts the same variables as `start_trading_ui.sh` and
applies them with the same precedence (configuration file first, environment fills gaps,
`UI_SSL_ENABLED` and `MARKET_INSIGHTS_MAX_LEVELS` override the file):

```
ACCOUNT_DASHBOARD_CONFIG   UI_HOST  UI_PORT  UI_SSL_ENABLED  UI_SSL_CERTFILE  UI_SSL_KEYFILE  UI_TLS_NAME
UI_REDIRECT_ENABLED  UI_REDIRECT_HOST  UI_REDIRECT_PORT  UI_REDIRECT_TARGET_HOST  UI_REDIRECT_TARGET_PORT
KAFKA_BOOTSTRAP_SERVERS  KAFKA_MARKET_DATA_BOOTSTRAP_SERVERS  KAFKA_ACCOUNT_DETAILS_TOPIC
KAFKA_TRADING_COMMANDS_TOPIC  KAFKA_MARKET_DATA_TOPIC  KAFKA_STOCK_QUOTES_TOPIC  KAFKA_GROUP_ID
KAFKA_MARKET_DATA_GROUP_ID  KAFKA_AUTO_OFFSET_RESET  KAFKA_MARKET_DATA_AUTO_OFFSET_RESET
MARKET_INSIGHTS_MAX_LEVELS  OPERATIONS_LOG_DIR  LOG_LEVEL
```

Put them in `/etc/nextgen-gui.env` when running under systemd.

## 4. Switch over

1. Stop the Python service (`start_trading_ui.sh` kills older instances itself; with systemd
   use `systemctl stop <old unit>`). Do this before starting the new server if both share the
   Smart selling journal — the second owner is refused by the journal lock.
2. Start `nextgen-gui`:
   ```bash
   sudo cp deploy/nextgen-gui.service /etc/systemd/system/
   sudo systemctl daemon-reload && sudo systemctl enable --now nextgen-gui
   journalctl -u nextgen-gui -f
   ```
3. Watch the feed pills in the top bar (or `GET /api/health`): `account-details` and
   `market-data` go `live` as soon as the first messages arrive, the Smart selling
   `orders`/`volume` feeds go `replaying` → `live` after the retained order-status and
   trading-volume topics have been replayed.
4. Log in with the same users and passwords. Sessions are in-memory, as before, so everyone
   signs in again after a restart.

Rolling back is the reverse: stop `nextgen-gui`, start the Python service. The journal is
shared and the trading-commands topic has not changed format.

## 5. Kafka consumer groups

The new server uses the configured group ids (`kafka.group_id`,
`kafka.market_data_group_id`) exactly like the Python service, so it inherits committed
offsets for `account-details`. Market data and the Smart selling feeds always restart from
the beginning of the retained log (`OFFSET_BEGINNING`), matching the previous behaviour. If
you run both servers side by side for comparison, give the new one distinct group ids and a
distinct `state_db`.

## 6. What changed for browsers and scripts

* All pages are one client-side application. Deep links and the navigation are the same;
  the server no longer renders HTML tables, so bookmarks work but screen-scraping the old
  templates does not.
* `/api/account-details/stream` sends JSON events instead of HTML fragments. The new
  multiplexed stream `/api/stream?topics=accounts,quotes,smart,algo,feeds,commands` is what
  the UI uses; see `docs/API.md`.
* Legacy form endpoints (`/submit-order`, `/submit-algo`, ...) still redirect back with
  `?ok=` / `?err=` and the UI shows those as toasts.
* The `lang` cookie is still shared with `order_ui`. Logins and entitlements are unchanged: the
  users in `auth.users` (`admin` and `trader` mandatory) can sign in, and only `lmh` can use
  currency conversion and trading-status controls.

## 7. Persistence

The daily order files (`~/Reports/orders-<date>/orders-<date>.json` and
`source-snapshots.json`) are now produced by the server itself at the configured local time,
or on demand with `nextgen-gui --persist-orders [--date YYYY-MM-DD]`; point
`persistence.order_archive.dir` (or `ORDER_ARCHIVE_DIR`) at the directory the old script used.
Account holdings are recorded every minute under `persistence.account_snapshots.dir`
(`ACCOUNT_SNAPSHOTS_DIR`), as JSON lines per day plus a SQLite database.

## 8. Not migrated (by design)

`order_ui/`, `admin_ui/`, `configurator_ui/`, `trading_server/` (Tiger and Futu listeners),
and the market-data producers. They were explicitly out of scope and need no change.
