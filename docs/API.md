# HTTP API

All endpoints are served by `nextgen-gui`. Unless noted, `/api/*` requires the `auth_token`
cookie obtained from `POST /login` and answers `401 {"ok": false, "error": "Unauthorized"}`
otherwise. `POST /api/*` additionally requires a same-origin `Origin` header when one is sent
(`403` otherwise). Request bodies may be JSON (`application/json`), URL-encoded forms or
multipart forms; responses are JSON. Messages are localised by the `lang` cookie (`en`/`zh`)
or `?lang=`.

## Pages and session

| Method | Path | Notes |
| --- | --- | --- |
| GET | `/` | 302 → `/account-details` |
| GET | `/account-details`, `/control-panel`, `/market-data`, `/market-insights`, `/currency-conversion`, `/trading-status`, `/login` | Single-page application shell |
| GET | `/static/{file}`, `/favicon.svg`, `/favicon.ico` | Embedded assets (or `--static-dir`) |
| GET | `/set-lang?lang=en|zh&next=/path` | Sets the 180-day `lang` cookie (shared with `order_ui`), redirects |
| POST | `/login` | Form `username`, `password` (+ optional `next`). 303 to `next` with a 12 h `auth_token` cookie (HttpOnly, SameSite=Lax, Secure under TLS). With `Accept: application/json`: `{"ok": true, "user": "...", "next": "..."}` or 401 |
| GET | `/logout` | Revokes the session, 303 → `/login` |
| POST | `/api/logout` | Revokes the session, `{"ok": true}` |
| GET | `/api/session` | `{"authenticated": bool, "user", "privileged", "lang"}` (never 401 for the unauthenticated case — returns 401 with `authenticated: false`) |
| GET | `/api/i18n?lang=` | Full translation dictionary for the UI |
| GET | `/api/bootstrap` | Everything the SPA needs at start: user, privileges, account metas, symbols, fast-trading levels, Smart selling defaults/participation percentages, instrument rules, version |

## Read APIs

| Method | Path | Response |
| --- | --- | --- |
| GET | `/api/accounts` | `{"accounts": [snapshot...], "lang", "source": "kafka"}` — the Python shape |
| GET | `/api/accounts/view` | Dashboard view: accounts with `meta`, `has_snapshot`, `stale`, portfolio totals, stock quotes, `version`, `generated_at` |
| GET | `/api/market-data?symbol=TSLA` (repeatable) | `{"rows": {SYM: {symbol, prev_close, last, change, change_pct, short_interest, volume, asof_epoch, error}}}` |
| GET | `/api/market-insights?symbol=TSLA&depth=20` | `{symbol, symbols, depth_limit, asof_epoch, error, bids: [{price, quantity, order_count}], asks: [...]}` |
| GET | `/api/algo` | `{"active": bool, "fast": [strategy statuses], "smart": {...}}` |
| GET | `/api/smart-selling` | `{"ok": true, "available": bool, "feeds": {"orders", "volume"}, "parents": [parent status + market_state + reason]}` |
| GET | `/api/smart-selling/{parent_id}/events?limit=100` | `{"ok": true, "events": [{id, ts, kind, data}]}` (newest first; empty for unknown ids) |
| GET | `/api/commands` | The last 200 published trading commands (newest first) with delivery status |
| GET | `/api/operations` | Current operations-log document (algorithm sessions, events) |
| GET | `/api/health` | Feed health (`account-details`, `market-data`, `orders`, `volume`), producer stats, SSE subscriber count, version, uptime, and `persistence` (order archive: next run, last result; holdings recorder: count, last tick, last error) |

## Command APIs

All command endpoints return `{"ok": true, "message": ..., ...}` or `{"ok": false, "error": ...}`
with HTTP 400 for validation errors, 403 when the user lacks the privilege, 500 when the
Kafka producer rejects the command. Published payloads are byte-compatible with the Python
service (see `trading_ui_kafka_command_schema_en.docx` in the KTrader repository).

| Method | Path | Body | Publishes |
| --- | --- | --- | --- |
| POST | `/api/orders/market` or `/api/submit-order` | `account_id`, `symbol`, `side`, `shares` | `MARKET_ORDER` |
| POST | `/api/orders/quick` | `symbol`, `side`, `shares`, `account_ids[]` | one `MARKET_ORDER` per account |
| POST | `/api/orders/limit` | `symbol`, `side`, `shares`, `limit_price`, `account_ids[]` | one `LIMIT_ORDER` per account |
| POST | `/api/orders/cancel` | `symbol`, `account_ids[]` | `CANCEL_OPEN_ORDERS` per account |
| POST | `/api/account-orders/quick-market` | `account_id`, `symbol`, `side`, `shares` | `MARKET_ORDER` (account card) |
| POST | `/api/account-orders/limit` | `account_id`, `symbol`, `side`, `shares`, `limit_price` | `LIMIT_ORDER` (account card) |
| POST | `/api/fx` | `account_id`, `source_currency`, `target_currency`, `source_amount` (HKD and USD only) | `CURRENCY_CONVERSION` (lmh only) |
| POST | `/api/trading-status` | `account_ids[]`, `enabled` | `SET_TRADING_ENABLED` (lmh only) |
| POST | `/api/submit-algo` | `trading_mode` (`E`/`F`), `symbol`, `fast_trading_*` settings (`account_ids`, `price_limit`, `book_levels`, `target_percentage`, `cycle_seconds`, `aggression_level`, `test_mode`) | `START_ALGO_TRADING` then `LIMIT_ORDER_FOK` children per cycle |
| POST | `/api/algo/stop` or `/submit-algo-stop` | optional `reason` | Stops every fast strategy and Smart selling parent, publishes `STOP_ALGO_TRADING`; `{"fast_stopped", "smart_stopped", "warning"?}` |
| POST | `/api/smart-selling` | `account_ids[]` (or `account_id`), `symbol`, optional `quantity`, `mode` (`patient`/`normal`/`urgent` or 1–3), `deadline` (ISO or null), `start_time`, `minimum_price` | Starts a mode-G parent; `{"ok": true, "parent": {...}}` |
| POST | `/api/smart-selling/{parent_id}/stop` | — | Requests a stop (pending children are cancelled through `CANCEL_OPEN_ORDERS`) |

Accounts owned by a running algorithm reject manual orders (`... owns this account`); a
running fast strategy blocks Smart selling and vice versa (`Stop the existing algorithm ...`).

### Legacy form endpoints

`POST /submit-order`, `/submit-quick-order`, `/submit-limit-order`,
`/submit-cancel-open-orders`, `/submit-currency-conversion`, `/submit-trading-status`,
`/submit-algo`, `/submit-algo-stop` accept the old form fields and answer `303` back to their
page with `?ok=` or `?err=`; `GET` on them redirects to the page. Unauthenticated posts are
redirected to `/login?next=...`.

## Server-sent events

`GET /api/stream?topics=accounts,quotes,smart,algo,feeds,commands[&book=SYM&depth=N]`

One connection carries every topic the client asks for. Each event is
`event: <topic>\ndata: <json>\n\n`; a full snapshot of every subscribed topic is sent on
connect, then updates are coalesced per client: `accounts` and `book` events at most 10 per
second, `quotes` at most 4 per second, the other topics at most 2 per second. A
`: keep-alive` comment goes out after 15 s without events.

| Topic | Payload |
| --- | --- |
| `accounts` | Same document as `/api/accounts/view` |
| `quotes` | `{"rows": {...}}` as `/api/market-data` for all configured symbols |
| `book` | `/api/market-insights` document for the requested `book` symbol and `depth` |
| `smart` | `/api/smart-selling` document |
| `algo` | `/api/algo` document |
| `feeds` | `/api/health` feeds section |
| `commands` | `/api/commands` document |

`GET /api/account-details/stream` is kept for compatibility and is equivalent to
`/api/stream?topics=accounts` (JSON payloads; the Python service streamed HTML here).

The stream re-checks the session every 5 s and closes when it has been revoked.
