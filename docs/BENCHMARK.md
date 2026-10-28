# C++ vs Python: measured results

The question behind the rewrite was whether a C++ server is a *clear* win over the existing
Python/FastAPI service. Both were run side by side on the same 12-thread Linux box against the
same local Apache Kafka 4.4 broker, fed by the same synthetic market/account traffic
(`tools/bench/feeder.py`: five symbols with 20-level books at 2 Hz each, five account snapshots
every 2 s, quotes and the volume tape at 1 Hz), and measured with `tools/bench/bench.py`
(`wrk` for HTTP, a 300-client stream reader, a 4-process Kafka burst) and
`tools/bench/stress.py` (simulated browser sessions). The Python service ran unchanged under
uvicorn from the KTrader checkout; the C++ server is the release build from this repository.

## HTTP endpoints

`wrk`, 15 s per endpoint, 64 connections (throughput) and 8 connections (latency), one logged-in
session cookie. Both servers served identical JSON documents (same accounts, same books).

| Endpoint | C++ req/s | Python req/s | C++ p50 / p99 (8 conn) | Python p50 / p99 (8 conn) |
| --- | ---: | ---: | --- | --- |
| `/api/accounts` | 442,000 | 4,430 | 0.02 / 0.04 ms | 1.99 / 2.84 ms |
| `/api/market-data` | 418,000 | 6,450 | 0.02 / 0.04 ms | 1.30 / 1.45 ms |
| `/api/market-insights` (20 levels) | 438,000 | 5,410 | 0.02 / 0.04 ms | 1.55 / 1.90 ms |
| `/account-details` page | 468,000 | 2,600 | 0.02 / 0.03 ms | 3.29 / 4.44 ms |
| `/market-data` page | 456,000 | 5,380 | 0.02 / 0.03 ms | 1.68 / 2.49 ms |

At 64 connections the C++ p99 stays under 0.7 ms on every endpoint while saturating 6–8 cores;
the Python process is pinned at one core (the interpreter lock) with 10–30 ms medians. The
page requests are not the same work: Python renders the HTML server-side (16–46 KB per
response), the C++ server returns the 1.6 KB application shell and the browser renders from
JSON — which is precisely why it was restructured that way.

Resident memory: C++ 25 MB idle, 27 MB under load (111 MB briefly while 64 connection threads
are alive, released again by the arena limit and periodic trim); Python 155 MB.

## Live streams (server-sent events)

300 concurrent stream clients for 15 s while the demo feed runs.

| | C++ | Python |
| --- | --- | --- |
| clients connected | 300 / 300 | 300 / 300 |
| events delivered | 19,749 (4.4 per client per second) | 7,445 (1.7 per client per second) |
| bytes | 99 MB (JSON documents) | 82 MB (HTML fragments) |
| process CPU | 32 % of one core | 12 % of one core |
| RSS | 55 MB | 157 MB |

The C++ server pushes every change (coalesced to at most 10 per second per topic) and the
documents are serialized once per data version and shared by all clients; the Python service
polls its store twice a second and re-renders HTML per client, which caps its update rate.
Before the shared serialization was added the same test cost 67 % of a core, and before the
elastic worker pool only 48 of the clients could connect at all (cpp-httplib's fixed pool).

## Kafka ingestion

Four feeder processes publish ~10,700 messages/s (price books, quotes, account snapshots,
volume) for 15 s while both servers consume the same topics.

| | C++ | Python |
| --- | --- | --- |
| process CPU during the burst | 37 % of one core | 83 % of one core |
| served snapshot age (max / mean) | 1.55 s / 0.16 s | 1.55 s / 0.16 s |
| consumer-group lag 2 s after the burst | 0 | 0–5 |

Both keep up with that rate (the feeder's own cadence dominates the snapshot age); the
difference is headroom. Both consumers are librdkafka underneath; the C++ side spends its time
in JSON parsing and store updates only.

## Simulated browser sessions (stress test)

`tools/bench/stress.py` drives whole sessions, not raw requests: every virtual user logs in,
keeps the multiplexed event stream open (half of them a second, order-book stream as on the
market-insights page), fetches a page API every 1–3 s, navigates between pages, and now and
then submits market/limit orders, cancels, FX conversions (the `lmh` users), fast-trading
starts, Smart selling starts and stop-all requests — a far more aggressive mix of algorithm
starts and stops than a trading day would see. While it runs, the server's CPU, memory, thread
and file-descriptor counts, its feed health, the age of the served account snapshots and the
Kafka consumer lag are sampled every 2 s.

| | 48 sessions, 90 s | 96 sessions, 60 s | 192 sessions, 60 s | 48 sessions + 5,000 msg/s burst |
| --- | --- | --- | --- | --- |
| requests (unexpected errors) | 2,647 (0) | 3,519 (0) | 7,062 (0) | 2,606 (0) |
| page / API GET p99 | ≤ 0.7 ms | ≤ 0.9 ms | ≤ 0.9 ms | ≤ 0.9 ms |
| order POST p99 | ≤ 1.0 ms | 7–12 ms | 5–9 ms | 2–10 ms |
| streams open / failed | 72 / 0 | 144 / 0 | 288 / 0 | 72 / 0 |
| stream events per client | 5.9 /s, max gap 0.5 s | 5.8 /s, max gap 1.0 s | 6.0 /s, max gap 0.6 s | 6.9 /s, max gap 0.5 s |
| server CPU (avg / max, one core = 100 %) | 13 % / 32 % | 36 % / 86 % | 39 % / 139 % | 28 % / 62 % |
| RSS / threads / fds | 55 MB / 152 / 169 | 83 MB / 271 / 287 | 138 MB / 509 / 525 | 135 MB / 509 / 169 |
| served snapshot age (max) | 1.97 s | 1.87 s | 1.89 s | 2.00 s |
| Kafka consumer lag (max) | 15 | 15 | 10 | 740 during the burst, 0 after |
| trading commands published / failed | 205 / 0 | 474 / 0 | 1,052 / 0 | 1,298 / 0 |

Every feed stayed `live` throughout, no stream ever stalled for more than a second, the
account snapshots never aged beyond the feeder's own 2 s cadence (also during the burst, when
the consumers briefly ran up to 740 messages behind and caught up within two seconds), and all
trading commands were delivered. The "rejected" submissions in the harness are the expected
refusals: only one algorithm session may run at a time, an account owned by a running
algorithm refuses manual orders, Smart selling refuses an instrument with open manual orders,
and currency conversion is limited to Tiger accounts — exactly the original rules.

What the test did find: `/api/health` and `/api/commands` had tail latencies of up to ~100 ms
at 96 sessions, and manual orders submitted while another user was starting or stopping an
algorithm waited behind it (up to ~300 ms at 192 sessions). Both come from the algorithm lock:
starting or stopping a strategy writes the Smart selling journal and the operations log with
`synchronous=FULL` (the same durability settings as the Python engine) and publishes the stop
notification, and manual orders must observe the result for the ownership check. Serializing
the status documents once per change (instead of once per request) cut the health p99 at 96
sessions from 95 ms to 12 ms; the remaining tail only appears under dozens of algorithm starts
per minute. The Python service, driven with the same 48-session mix on its own endpoints,
also stayed error-free (p99 1–12 ms) at 4 % of a core — the difference at this size is the
update rate of the live pages (1.6 vs 5.9 events per client per second) and headroom, not
correctness.

## Rerunning

```bash
# terminal 1: a Kafka broker on 127.0.0.1:9092 with the KTrader topics, then
python3 tools/bench/feeder.py --books-hz 2 --accounts-hz 0.5 --quotes-hz 1 --volume-hz 1
# terminal 2: both servers (distinct group ids and state_db), then
python3 tools/bench/bench.py --kafka-bin /path/to/kafka/bin --feeder tools/bench/feeder.py --python $(which python3) --out bench.json
python3 tools/bench/stress.py --base http://127.0.0.1:8765 --users 48 --duration 90 --kafka-bin /path/to/kafka/bin
```

`bench.py` needs `wrk`; `stress.py` needs `aiohttp`; the feeder needs `confluent_kafka`.
