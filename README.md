# nextgen-gui — KTrader control server, rewritten in C++20

`nextgen-gui` replaces the Python/FastAPI `trading_ui` service of
[mhliu94/KTrader](https://github.com/mhliu94/KTrader) with a single static binary:
the account dashboard, control panel (manual orders, fast-trading modes E/F, Smart selling
mode G), market data, market insights, currency conversion and trading-status pages, the
Kafka consumers/producer behind them, and a new single-page web UI.

It reads the **same configuration files** and the **same Kafka topics/message schemas** as the
Python service, publishes byte-compatible `trading-commands`, and opens the existing Smart
selling SQLite journal, so it can be dropped in where `start_trading_ui.sh` used to run.

Everything else in the repository (the order GUI `order_ui/`, the Tiger/Futu listeners in
`trading_server/`, the market-data producers, `admin_ui/`, `configurator_ui/`) is unchanged
and keeps working with this server.

## Why C++

The service is latency-sensitive glue between Kafka and a handful of browsers: it has to parse
every price-book/account snapshot the instant it arrives, keep the Smart selling controller's
1 Hz loop punctual while the HTTP side is busy, and never stall under a burst. The C++ build
has no interpreter lock, keeps all state in memory behind fine-grained locks, pushes UI
updates over a single multiplexed event stream instead of re-rendering HTML per client, and
links librdkafka/SQLite statically. Measured against the Python service on the same box
([docs/BENCHMARK.md](docs/BENCHMARK.md)): 80–180× the HTTP throughput with sub-millisecond
p99 latencies, 25 MB resident instead of 155 MB, a third of the CPU per ingested Kafka message,
and 192 simulated browser sessions (288 live streams, orders and algorithm starts included)
served without a single error at under 40 % of one core.

## Quick start (fresh Linux box)

```bash
git clone <this repository> ~/nextgen-gui
cd ~/nextgen-gui
./scripts/bootstrap.sh          # installs g++/cmake/ninja/openssl headers via apt/dnf/zypper/apk/pacman, builds, runs tests
cp config/config.example.json config/config.local.json
cp config/trading_accounts.example.json config/trading_accounts.json
$EDITOR config/config.local.json # passwords, Kafka bootstrap servers, symbols
./scripts/start_nextgen_gui.sh   # HTTPS on :443 with a self-signed cert + HTTP->HTTPS redirector on :80
```

Only a C++20 compiler (GCC 11 or newer; built and tested with GCC 11.5 and 15.2), CMake 3.22+,
OpenSSL headers and optionally zlib headers are needed from the system. librdkafka, SQLite, cpp-httplib, nlohmann/json and
doctest are compiled from the vendored sources in `third_party/`; the resulting binary links
`libstdc++`/`libgcc` statically and only needs the system OpenSSL, zlib and libc at runtime.

Manual build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/nextgen-tests            # 187 test cases, including Kafka round-trips on an in-process mock cluster
./build/nextgen-gui --check-config --config config/config.local.json
```

Useful CMake options: `-DNG_ENABLE_LTO=ON` (link-time optimisation),
`-DNG_STATIC_LIBSTDCXX=OFF`, `-DNG_BUILD_TESTS=OFF`.

## Running

`scripts/start_nextgen_gui.sh` understands the same environment variables as the Python
`start_trading_ui.sh` (`ACCOUNT_DASHBOARD_CONFIG`, `UI_HOST`, `UI_PORT`, `UI_SSL_ENABLED`,
`UI_SSL_CERTFILE`, `UI_SSL_KEYFILE`, `UI_TLS_NAME`, `UI_REDIRECT_*`, `KAFKA_*`,
`MARKET_INSIGHTS_MAX_LEVELS`, `OPERATIONS_LOG_DIR`), generates a self-signed certificate when
TLS is on, stops a previous instance, and `exec`s the binary. Run it with `--foreground` from
systemd. The binary itself takes:

```
nextgen-gui [--config PATH] [--host HOST] [--port PORT] [--ssl|--no-ssl]
            [--ssl-certfile PATH --ssl-keyfile PATH]
            [--redirect-port PORT --redirect-host HOST --redirect-target HOST]
            [--threads N] [--static-dir DIR] [--without-kafka] [--check-config] [--version]
```

`--static-dir web/static` serves the UI from disk instead of the copy embedded in the binary
(handy while editing `app.js`). `--without-kafka` starts the UI with no broker for demos.
`LOG_LEVEL=DEBUG|INFO|WARNING|ERROR` controls logging; everything goes to stderr in the same
`timestamp LEVEL [logger] message` layout as before, so `journalctl` output is unchanged.

### systemd

```bash
sudo cp deploy/nextgen-gui.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nextgen-gui
journalctl -u nextgen-gui -f
```

Edit `User`, `WorkingDirectory` and `ExecStart` in the unit if the checkout is not
`/home/ubuntu/nextgen-gui`; environment overrides go into `/etc/nextgen-gui.env`.

## Configuration

The configuration is the Python service's `config.local.json`, unchanged — see
[config/config.example.json](config/config.example.json). Resolution order:
`--config`, `$ACCOUNT_DASHBOARD_CONFIG`, `./config/config.local.json`, `./config/config.json`.
Relative paths inside the file (`accounts_file`, `historical_prices_csv`, `state_db`, TLS
files) resolve against the config file's directory, exactly as before.

| Section | Notes |
| --- | --- |
| `accounts_file` / `accounts` | `trading_accounts.json` (also written by `configurator_ui`); accounts with `"hidden": true` are skipped |
| `symbols` | Market-data symbols; `market_insights_max_levels` caps the ladder depth |
| `auth.users` | Plain-text dashboard logins, unchanged: `admin` and `trader` are mandatory, further users (such as `lmh`) are optional. Only the `lmh` account may use currency conversion and trading status, exactly as in the original |
| `server` | `host`, `port`, `ssl_enabled`, `ssl_certfile`, `ssl_keyfile` (env `UI_*` fills gaps; `UI_SSL_ENABLED` wins over the file) |
| `kafka` | Control cluster + market-data cluster bootstrap servers, topics, group ids, offset resets, producer settings, optional `client_config` / `market_data_client_config` with raw librdkafka properties (`security.protocol`, `sasl.*`, `ssl.*`) |
| `smart_selling` | Journal path, order-status/trading-volume topics (and optional separate bootstrap servers), freshness limits, participation percentages per mode, instrument rules |
| `fast_trading` | Aggression levels and per-medium minimum cycle seconds for modes E/F |
| `persistence` | `order_archive` (dir, timezone, daily_at, topic) and `account_snapshots` (dir, interval_seconds, formats); see Persistence below |

Validation errors produce the same messages as the Python loader
(`accounts_file ... not found`, `trading_accounts.json entry 3 is missing numeric_id`, ...).

## Architecture

```
src/core          Decimal (exact int128 arithmetic), config loader, i18n (EN/ZH table from the
                  Python service + new UI strings), logging, time, strings, SQLite wrapper
src/model         Account snapshots, market-data stores (quotes, 20-level books, stock quotes,
                  historical closes)
src/kafka         RAII librdkafka wrappers; account-details / price-books+stock-quotes
                  consumers; trading-commands producer with delivery tracking; replay-gated
                  order-status and trading-volume feeds for Smart selling
src/orders        Command builders and validators (MARKET/LIMIT/LIMIT_FOK/CANCEL/FX/
                  trading-status/algo start+stop), identical ids and payloads
src/algo          The algorithm trading modes, kept apart from the plumbing:
  fast_trading/     Modes E/F: cycle planner + strategy manager (cooldowns, reservations,
                    auto-stop after 10 capacity no-ops, operations log)
  smart_selling/    Mode G: durable fixed-POV engine (same SQLite schema and JSON ledger
                    layout as the Python engine), market adapter, order-status feedback,
                    runtime with 1 Hz heartbeat and single publisher worker
src/web           Session store, control center (all views and command paths), event hub
                  (coalesced SSE fan-out), HTTP server (cpp-httplib, TLS, legacy form
                  endpoints, JSON API, SSE multiplex)
web/static        Single-page UI (vanilla JS, no build step), embedded into the binary
tests/            doctest suite: unit tests, HTTP tests on a live server, Kafka tests on
                  librdkafka's mock cluster; tests/algo/ holds the ports of the Python
                  fast-trading and Smart selling scenario suites
tools/algo_oracle Differential test drivers (C++ and Python) and the random scenario diff
tools/bench       Kafka feeder, HTTP/SSE/Kafka benchmark and the browser-session stress test
```

Threads at runtime: one HTTP acceptor + worker pool (`--threads`, default
`max(16, 4 × cores)`), one thread per Kafka consumer (account-details, market-data,
order-status, trading-volume), the producer's delivery-report thread, the Smart selling
heartbeat and publisher threads, a 1 Hz status notifier, and one worker per running fast
strategy. All of that comfortably fits a 6c/12t box with room to spare for the browsers.

## Web UI

The UI is a single page served for every route (`/account-details`, `/control-panel`,
`/market-data`, `/market-insights`, `/currency-conversion`, `/trading-status`, `/login`). It
keeps the previous navigation and forms but renders client-side from JSON and a single
multiplexed `text/event-stream` (`/api/stream`), so a price-book burst costs one coalesced
event per client instead of a re-rendered HTML document. Features: dark/light theme, EN/中文
toggle (shared `lang` cookie with `order_ui`), live feed-health pills and health dialog,
account cards with per-account Market/Limit order modals, fast-trading (E/F) and Smart
selling (G) launchers with confirmation, active-algorithm table with stop/stop-all,
sparklines on the market-data table, and a bid/ask ladder with depth selector on market
insights. Legacy form posts still work and surface their `?ok=`/`?err=` results as toasts.

The layout is responsive: on phones the top bar collapses to the brand, feed dots and
controls with a scrollable tab row, KPI tiles go two-up, cards and forms stack, tables scroll
inside their cards, dialogs become bottom sheets and controls grow to touch size. A 66 KB
subset of Noto Sans CJK SC is embedded as a last-resort font so the Chinese UI renders even on
machines without a Chinese system font.

## Verification

* `build/nextgen-tests` — 187 doctest cases (2,891 assertions): unit tests for every module,
  ports of the Python Smart selling scenario suites (engine, market adapter, order-status
  feedback, runtime) and of the fast-trading planner tests, HTTP tests against a live server,
  and Kafka tests against librdkafka's in-process mock cluster. The suite also builds and
  passes with GCC 11.5.
* **Differential tests against the original implementation.** `tools/algo_oracle/` drives the
  same randomly generated scenarios through this port (`nextgen-algo-driver`) and through
  the Python `trading_ui.services` code, then compares the traces step by step: every action
  the engine emits, every ledger field (`earned`, `free_credit`, `reserved`, `filled`,
  `discarded`, per-account inventory, order state, fallback, reasons), market-adapter
  verdicts (acceptance, market state, quote health, volume rate), feedback correlation and
  the fast-trading planner's output for modes E and F.

  ```bash
  python3 tools/algo_oracle/oracle_diff.py --cpp-driver build/nextgen-algo-driver \
      --python /path/to/venv/bin/python --repo /path/to/KTrader --count 400 --seed 2
  ```

  Last run: 650 scenarios / 18,476 compared steps across three seeds (one of them with
  strict reason/error text comparison), zero divergences.

## Persistence

Two recorders run inside the server; both locations are configurable in the `persistence`
section (and by environment: `ORDER_ARCHIVE_DIR`, `ACCOUNT_SNAPSHOTS_DIR`).

**End-of-day order archive.** Once a day at `persistence.order_archive.daily_at` (local time
of `timezone`, default 17:00 America/New_York) the server reads the retained `order-status`
topic from its beginning, keeps every valid snapshot of orders *created* on that trading day
(day boundaries in the configured zone, like the order dashboard), and writes

```
<dir>/orders-<date>/orders-<date>.json      latest state per order + broker/medium + New York times
<dir>/orders-<date>/source-snapshots.json   every matching raw update with topic/partition/offset
```

in the layout of the daily files on the trading box (the `saved_order_export` and
`saved_order_snapshots` sources of the order-history export). The run can also be triggered
by hand or from cron:

```bash
./build/nextgen-gui --persist-orders --config config/config.local.json            # today
./build/nextgen-gui --persist-orders --date 2026-10-21 --config config/config.local.json
```

**Account holdings snapshots.** Every `persistence.account_snapshots.interval_seconds`
(default 60, aligned to the wall clock) the current holdings of every configured account —
positions with quantity, available quantity and average price, cash and available cash by
currency, trading state, and the age of the broker snapshot behind it — are appended to
`<dir>/holdings-<local date>.jsonl` (one JSON document per tick) and inserted into
`<dir>/holdings.sqlite3` (tables `snapshots`, `account_states`, `holdings`, `cash`). Either
format can be switched off with `formats`. `GET /api/health` reports both recorders under
`persistence`.

## Compatibility notes

* Kafka messages in and out are unchanged; command ids keep the
  `cmd_|limit_|cancel_open_|fx_|trading_status_|algo_|algo_stop_` formats.
* The Smart selling journal (`smart_selling.sqlite3`) and its companion feedback store are
  opened as-is; a parent started by the Python service is reconciled by this server after a
  restart, and vice versa. The journal is `flock`ed so two servers cannot own it at once.
* `/api/account-details/stream` now emits JSON (`event: accounts`) rather than HTML
  fragments; every other JSON endpoint keeps its response shape. See
  [docs/API.md](docs/API.md).
* HTML is no longer rendered server-side, so anything that scraped the old templates must
  use the JSON API instead.
* `DELAYED_MARKET_ORDER` is not offered (it was already removed from the Python UI).

## Documentation

* [docs/MIGRATION.md](docs/MIGRATION.md) — moving a running Python deployment over.
* [docs/API.md](docs/API.md) — HTTP endpoints, payloads, SSE topics.
* [docs/BENCHMARK.md](docs/BENCHMARK.md) — measured C++ vs Python results and how to rerun.
