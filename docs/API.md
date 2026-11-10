# HTTP API

Two servers share the configuration file:

* `nextgen-gui`, the control server (replaces `trading_ui`): the sections below.
* `nextgen-orders`, the order monitor (replaces `order_ui`): see [Order monitor](#order-monitor-nextgen-orders) at the end.

Unless noted, the control server's `/api/*` requires the `auth_token` cookie obtained from
`POST /login` and answers `401 {"ok": false, "error": "Unauthorized"}` otherwise. `POST /api/*`
additionally requires a same-origin `Origin` header when one is sent (`403` otherwise). Request
bodies may be JSON (`application/json`), URL-encoded forms or multipart forms; responses are
JSON. Messages are localised by `?lang=` or the `lang` cookie (`en`/`zh`), in that order.

## Pages and session

| Method | Path | Notes |
| --- | --- | --- |
| GET | `/` | 302 → `/account-details` |
| GET | `/account-details`, `/control-panel`, `/market-insights`, `/trading-status`, `/login` | Single-page application shell |
| GET | `/market-data`, `/admin-control` | Application shell for `lmh`; other signed-in users get HTTP 403 with a plain-text message; anonymous visitors get the shell, which shows the sign-in form |
| GET | `/currency-conversion` | 404: the page was replaced by Admin Control upstream |
| GET | `/static/{file}`, `/favicon.svg`, `/favicon.ico` | Embedded assets (or `--static-dir`) |
| GET | `/set-lang?lang=en|zh&next=/path` | Sets the 180-day `lang` cookie (shared with the order monitor), redirects |
| POST | `/login` | Form `username`, `password` (+ optional `next`). 303 to `next` with a 12 h `auth_token` cookie (HttpOnly, SameSite=Lax, Secure under TLS). With `Accept: application/json` or a JSON body: `{"ok": true, "user": "...", "next": "..."}` or 401 |
| GET | `/logout` | Revokes the session, 302 → `/login` |
| POST | `/api/logout` | Revokes the session, `{"ok": true}` |
| GET | `/api/session` | `{"authenticated": bool, "user", "privileged", "can_access_admin_control", "can_access_market_data", "lang", "server_time", "version"}`; 401 with `authenticated: false` when signed out |
| GET | `/api/i18n?lang=` | Full translation dictionary for the UI |
| GET | `/api/bootstrap` | Everything the SPA needs at start: user, entitlements (`can_access_admin_control`, `can_access_market_data`, `can_manage_trading`), account metas, symbols, `market_data_symbols`, fast-trading levels and `target_percentage_choices`, Smart selling/buying defaults and participation rates, `smart_buying_available`, instrument rules, version |

## Read APIs

| Method | Path | Response |
| --- | --- | --- |
| GET | `/api/accounts` | `{"accounts": [snapshot...], "lang", "source": "kafka"}` — the Python shape |
| GET | `/api/accounts/view` | Dashboard view: accounts with `meta`, `has_snapshot`, `stale`, portfolio totals (cash by currency, estimated positions value), stock quotes, `version`, `generated_at` |
| GET | `/api/quotes[?symbol=TSLA]` | Live last-trade quotes: `{"rows": {SYM: {symbol, prev_close, last, change, change_pct, short_interest, volume, asof_epoch, error}}}`. `error` is `no live quote yet` or `quote feed is stale` (older than 30 s); prices are then `null` |
| GET | `/api/market-data?symbol=TSLA` | **lmh only** (403 otherwise; 400 for a symbol outside `market_data_symbols`; 503 when unavailable). Five-minute windows: `{ok, symbols, symbol, generated_at, timezone, order_feed, quote_state, last_trade: {time, quantity, price}, windows: [{start, end, current, market_volume, vwap_estimate, market_complete, bought, sold, gross, our_complete}]}` — see [MARKET_DATA.md](MARKET_DATA.md) |
| GET | `/api/market-insights?symbol=TSLA&depth=20` | `{symbol, symbols, depth_limit, asof_epoch, error, bids: [{price, quantity, order_count}], asks: [...]}` |
| GET | `/api/algo` | `{"active": bool, "fast": [session statuses], "smart": {...}, "buying": {...}}` |
| GET | `/api/smart-selling` | `{"ok": true, "available": bool, "feeds": {"orders", "volume"}, "parents": [parent status + market_state + reason]}` (recent parents plus every active one) |
| GET | `/api/smart-selling/{parent_id}/events?limit=100` | `{"ok": true, "events": [{id, ts, kind, data}]}` (newest first; empty for unknown ids) |
| GET | `/api/smart-buying` | Same shape for mode H; parents carry per-account `cash_budget` and `cash_spent` |
| GET | `/api/smart-buying/{parent_id}/events?limit=100` | As for Smart selling |
| GET | `/api/commands` | The last 200 published trading commands (newest first) with delivery status |
| GET | `/api/operations` | Current operations-log document (algorithm sessions, events) |
| GET | `/api/health` | Feed health (`account-details`, `market-data`, `orders`, `volume`), producer stats, SSE subscriber count, version, uptime, and `persistence` (`order_archive`, `account_snapshots`, `system_snapshots`) |

## Command APIs

All command endpoints return `{"ok": true, "message": ..., ...}` or `{"ok": false, "error": ...}`
with HTTP 400 for validation errors, 403 when the user lacks the entitlement, 500/503 when the
Kafka producer rejects the command. Published payloads are byte-compatible with the Python
service (see `trading_ui_kafka_command_schema_en.docx` in the KTrader repository, and
[ADMIN_CONTROL.md](ADMIN_CONTROL.md) for `GET_BROKER_ORDERS`).

| Method | Path | Body | Publishes |
| --- | --- | --- | --- |
| POST | `/api/orders/market` or `/api/submit-order` | `account_id`, `symbol`, `side`, `shares` | `MARKET_ORDER` |
| POST | `/api/orders/quick` | `symbol`, `side`, `shares`, `account_ids[]` | one `MARKET_ORDER` per account |
| POST | `/api/orders/limit` | `symbol`, `side`, `shares`, `limit_price`, `account_ids[]` | one `LIMIT_ORDER` per account |
| POST | `/api/orders/cancel` | `symbol`, `account_ids[]` | `CANCEL_OPEN_ORDERS` per account |
| POST | `/api/account-orders/quick-market` | `account_id`, `symbol`, `side`, `shares` | `MARKET_ORDER` (account card) |
| POST | `/api/account-orders/limit` | `account_id`, `symbol`, `side`, `shares`, `limit_price` | `LIMIT_ORDER` (account card) |
| POST | `/api/broker-orders` | `trading_date` (`YYYY-MM-DD`, today or earlier in New York), `account_ids[]` (`__ALL__` = every configured account) | one `GET_BROKER_ORDERS` (lmh only); `{"ok", "message", "command"}` |
| POST | `/api/trading-status` | `account_id` + `trading_enabled`, or `disable_all` | `SET_TRADING_ENABLED` (lmh only) |
| POST | `/api/submit-algo` | `trading_mode` (`E`/`F`), `symbol`, `fast_trading_config` / `fast_trading_*` settings (`account_ids`, `price_limit`, `book_levels`, `target_percentage` 10–100 in steps of 10, 120 or 150, `cycle_seconds`, `aggression_level`, `test_mode`, optional `total_target_position`) | `START_ALGO_TRADING`, then `LIMIT_ORDER_FOK` children per cycle |
| POST | `/api/algo/stop` or `/submit-algo-stop` | optional `reason` | Stops every fast session and every Smart selling and Smart buying parent, publishes `STOP_ALGO_TRADING`; `{"fast_stopped", "smart_stopped", "buying_stopped", "warning"?}` |
| POST | `/api/smart-selling` | `account_ids[]`, `symbol`, optional `quantity`, `mode` (`patient`/`normal`/`urgent`/`ultra` or 1–4), `deadline` (ISO or null), `start_time`, `minimum_price`, `disable_child_cap` (boolean) | Starts a mode-G parent; `{"ok": true, "parent": {...}}` |
| POST | `/api/smart-selling/{parent_id}/stop` | — | Requests a stop (working children are cancelled; 404 for unknown ids) |
| POST | `/api/smart-buying` | `account_ids[]`, `symbol`, `mode` (`patient`/`normal`/`urgent` or 1–3), `deadline`, `start_time`, `maximum_price` | Starts a mode-H parent sized from available USD cash and the best ask — see [SMART_BUYING.md](SMART_BUYING.md) |
| POST | `/api/smart-buying/{parent_id}/stop` | — | Requests a stop (404 for unknown ids) |

Several algorithm sessions may run at once as long as no account is shared: a fast session,
a Smart selling parent or a Smart buying parent owns its accounts until its orders reach a
final status. Starting another session on an owned account fails with
`Account(s) A, B already belong to an algorithm session; stop it and await outstanding order
confirmations before starting another`, and manual orders on an owned account fail with
`An algorithm session owns this account; stop it and await outstanding order confirmations first`.

### Legacy form endpoints

`POST /submit-order`, `/submit-quick-order`, `/submit-limit-order`,
`/submit-cancel-open-orders`, `/submit-broker-orders`, `/submit-trading-status`,
`/submit-algo`, `/submit-algo-stop` accept the old form fields and answer `303` back to their
page with `?ok=` or `?err=`. `GET` on them redirects to the page, except
`GET /submit-broker-orders`, which answers `405` (a history request is only queued by a POST).
Unauthenticated posts are redirected to `/login?next=...`. `/submit-currency-conversion` and
`/api/fx` no longer exist.

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
| `quotes` | `{"rows": {...}}` as `/api/quotes` for all configured symbols |
| `book` | `/api/market-insights` document for the requested `book` symbol and `depth` |
| `smart` | `/api/smart-selling` document |
| `algo` | `/api/algo` document (fast sessions, Smart selling and Smart buying) |
| `feeds` | `/api/health` feeds section |
| `commands` | `/api/commands` document |

`GET /api/account-details/stream` is kept for compatibility and is equivalent to
`/api/stream?topics=accounts` (JSON payloads; the Python service streamed HTML here).

The stream re-checks the session every 5 s and closes when it has been revoked.

## Order monitor (`nextgen-orders`)

A separate, read-only server (default port 8030) with its own sign-in for the same
`auth.users`. Every response carries `Cache-Control: no-store`, `X-Content-Type-Options:
nosniff`, `X-Frame-Options: DENY` and a Content-Security-Policy that only allows scripts,
styles and connections from the server itself. POST requests must carry the header
`X-Order-UI-Request: 1` and, when the browser sends one, an `Origin` equal to the server's own
(403 otherwise). Errors use FastAPI's shape, `{"detail": "..."}`.

| Method | Path | Notes |
| --- | --- | --- |
| GET | `/`, `/static/{file}` | The monitor page and its assets (`--static-dir DIR` serves `DIR/orders/` plus the shared `fonts/` and `favicon.svg`) |
| POST | `/api/login` | JSON `{"username" (1–256 chars), "password" (1–4096)}`; sets the 12 h cookie `ktrader_order_ui_session` (HttpOnly, SameSite=Strict, Secure under TLS); 401 `Incorrect username or password.`, 422 for a malformed body |
| POST | `/api/logout` | Drops the session, `{"ok": true}` |
| GET | `/api/orders` | `{date, timezone, generated_at, feed: {state, detail, last_message_at, invalid_messages, ignored_messages}, orders: [order snapshot + account_num_id, broker, trading_medium, tags: [{id, name}]], username, refresh_interval_seconds, statuses, accounts: [{id, num_id, broker}], reporting: {sessions, tags, active_tag_id}, unreconciled: {generated_at, complete, details_available, sources, items}}`; 401 `Sign in with your KTrader credentials.` |
| POST | `/api/tags` | JSON `{"name" (1–100), "symbol" (1–64)}` → 201 `{"id"}`; 409 while another tag is active, 422 for invalid input |
| POST | `/api/tags/{id}/stop` | `{"ok": true}`; stopping twice is harmless; 404 `Tag not found.` |

Session reports never include participation rates (`participation`, `participation_percentage`
and `participation_percentages` are removed from the starting parameters at any depth). See
[ORDER_MONITOR.md](ORDER_MONITOR.md) for what the reports contain.
