#!/usr/bin/env python3
"""Browser-session stress test for the KTrader control server.

Each virtual user behaves like an open browser tab: it logs in, keeps the live event stream(s)
open, polls the page APIs the way the UI does, navigates between pages and now and then
submits orders or starts/stops algorithms. Meanwhile the server process (RSS, CPU, threads,
file descriptors), its feed health, the freshness of served account snapshots and the Kafka
consumer lag are sampled, so a bottleneck outside the HTTP layer shows up too.
"""
import argparse
import asyncio
import json
import os
import random
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone

import aiohttp

SYMBOLS = ["TSLA", "NVDA", "AEHL", "BIYA", "POM"]
ACCOUNTS = ["TIGER_MAIN", "TIGER_ALT", "FUTU_HK", "GIGA_EMU", "IBKR_WIN"]

EXPECTED_REJECTIONS = ("Stop the existing", "owns this account", "already owns", "Insufficient", "insufficient",
                       "exceeds", "must reach terminal", "stale", "disabled", "No algorithm", "not running",
                       "Smart selling already", "no available position", "Wait for the order-status",
                       "available position", "Quantity", "forbidden", "only available")


def parse_iso(ts):
    return datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp()


class Stats:
    def __init__(self):
        self.lat = {}
        self.errors = {}
        self.rejected = {}
        self.rejection_samples = {}
        self.sse = {"opened": 0, "events": 0, "bytes": 0, "failed": 0, "max_gap": 0.0, "gaps_over_5s": 0, "closed_early": 0}
        self.samples = []
        self.staleness = []
        self.lag = []
        self.health = []

    def record(self, label, seconds, status, body=None):
        self.lat.setdefault(label, []).append(seconds)
        if status >= 500 or status == 0:
            text = (body or "")[:120]
            if any(k in text for k in EXPECTED_REJECTIONS):
                self.rejected[label] = self.rejected.get(label, 0) + 1
                self.rejection_samples.setdefault(label, text)
            else:
                self.errors.setdefault(label, []).append(f"{status} {text}")
        elif status >= 400 and status not in (401, 403, 404):
            self.rejected[label] = self.rejected.get(label, 0) + 1
            self.rejection_samples.setdefault(label, (body or "")[:120])
        elif status in (401, 403, 404):
            text = (body or "")[:120]
            if any(k in text for k in EXPECTED_REJECTIONS) or status == 403:
                self.rejected[label] = self.rejected.get(label, 0) + 1
            else:
                self.errors.setdefault(label, []).append(f"{status} {text}")


def pct(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    k = min(len(values) - 1, int(round((len(values) - 1) * p)))
    return values[k]


async def login(session, base, user, password):
    async with session.post(base + "/login", data={"username": user, "password": password}, allow_redirects=False) as r:
        return r.status in (303, 302) and "auth_token" in r.cookies


async def timed(session, stats, label, method, url, **kw):
    t0 = time.perf_counter()
    try:
        async with session.request(method, url, timeout=aiohttp.ClientTimeout(total=15), **kw) as r:
            body = await r.text()
            stats.record(label, time.perf_counter() - t0, r.status, body)
            return r.status, body
    except Exception as exc:  # noqa: BLE001
        stats.record(label, time.perf_counter() - t0, 0, str(exc))
        return 0, str(exc)


async def stream(session, stats, url, stop_at, label):
    """Reads one server-sent-events stream until stop_at, tracking gaps between events."""
    stats.sse["opened"] += 1
    try:
        async with session.get(url, timeout=aiohttp.ClientTimeout(total=None, sock_read=40)) as r:
            if r.status != 200:
                stats.sse["failed"] += 1
                return
            last = time.perf_counter()
            buffer = b""
            async for chunk in r.content.iter_any():
                now = time.perf_counter()
                stats.sse["bytes"] += len(chunk)
                buffer += chunk
                while b"\n\n" in buffer:
                    frame, buffer = buffer.split(b"\n\n", 1)
                    if frame.startswith(b":"):
                        continue  # keep-alive comment
                    stats.sse["events"] += 1
                    gap = now - last
                    last = now
                    if gap > stats.sse["max_gap"]:
                        stats.sse["max_gap"] = gap
                    if gap > 5:
                        stats.sse["gaps_over_5s"] += 1
                if now >= stop_at:
                    return
            if time.perf_counter() < stop_at - 1:
                stats.sse["closed_early"] += 1
    except asyncio.CancelledError:
        raise
    except Exception as exc:  # noqa: BLE001
        if time.perf_counter() < stop_at - 1:
            stats.sse["failed"] += 1
            stats.errors.setdefault(label, []).append(str(exc)[:100])


async def user(index, args, stats, stop_at):
    rng = random.Random(index * 7919 + args.seed)
    privileged = index % 8 == 0
    name, password = ("lmh", args.lmh_password) if privileged else ("trader", args.password)
    connector = aiohttp.TCPConnector(limit=0)
    async with aiohttp.ClientSession(connector=connector, cookie_jar=aiohttp.CookieJar(unsafe=True)) as session:
        t0 = time.perf_counter()
        ok = await login(session, args.base, name, password)
        stats.record("login", time.perf_counter() - t0, 200 if ok else 0, "" if ok else "login failed")
        if not ok:
            return
        streams = []
        if args.legacy:
            streams.append(asyncio.create_task(stream(session, stats, args.base + "/api/account-details/stream", stop_at, "sse")))
        else:
            streams.append(asyncio.create_task(stream(session, stats, args.base + "/api/stream?topics=accounts,quotes,smart,algo,feeds,commands", stop_at, "sse")))
            if index % 2 == 0:
                sym = rng.choice(SYMBOLS)
                streams.append(asyncio.create_task(stream(session, stats, f"{args.base}/api/stream?topics=book&book={sym}&depth=20", stop_at, "sse-book")))
        await asyncio.sleep(rng.uniform(0, 2))
        pages = ["/account-details", "/control-panel", "/market-data", "/market-insights"]
        apis = [("accounts_view", "/api/accounts/view"), ("accounts", "/api/accounts"), ("market_data", "/api/market-data"),
                ("insights", "/api/market-insights?symbol=TSLA&depth=20"), ("smart", "/api/smart-selling"), ("algo", "/api/algo"),
                ("health", "/api/health"), ("commands", "/api/commands")]
        if args.legacy:
            apis = [("accounts", "/api/accounts"), ("market_data", "/api/market-data?symbol=TSLA"),
                    ("insights", "/api/market-insights?symbol=TSLA&depth=20"), ("smart", "/api/smart-selling")]
        while time.perf_counter() < stop_at:
            r = rng.random()
            if r < 0.65:
                label, path = rng.choice(apis)
                await timed(session, stats, "get:" + label, "GET", args.base + path)
            elif r < 0.85:
                page = rng.choice(pages)
                await timed(session, stats, "page", "GET", args.base + page)
                if not args.legacy:
                    await timed(session, stats, "get:bootstrap", "GET", args.base + "/api/bootstrap")
            else:
                await post_action(session, stats, rng, args, privileged)
            await asyncio.sleep(rng.uniform(args.min_interval, args.max_interval))
        for task in streams:
            task.cancel()
        await asyncio.gather(*streams, return_exceptions=True)


async def post_action(session, stats, rng, args, privileged):
    headers = {"Origin": args.base, "Content-Type": "application/json"}
    account = rng.choice(ACCOUNTS)
    symbol = rng.choice(SYMBOLS)
    x = rng.random()
    if x < 0.4:
        body = {"account_id": account, "symbol": symbol, "side": rng.choice(["BUY", "SELL"]), "shares": rng.choice([1, 10, 100])}
        await timed(session, stats, "post:market_order", "POST", args.base + "/api/submit-order", json=body, headers=headers)
    elif x < 0.6:
        body = {"account_id": account, "symbol": symbol, "side": "BUY", "shares": 100, "limit_price": 1.0}
        await timed(session, stats, "post:limit_order", "POST", args.base + "/api/account-orders/limit", json=body, headers=headers)
    elif x < 0.7 and not args.legacy:
        body = {"symbol": symbol, "account_ids": [account]}
        await timed(session, stats, "post:cancel", "POST", args.base + "/api/orders/cancel", json=body, headers=headers)
    elif x < 0.8 and privileged and not args.legacy:
        body = {"account_id": account, "source_currency": "HKD", "target_currency": "USD", "source_amount": 100}
        await timed(session, stats, "post:fx", "POST", args.base + "/api/fx", json=body, headers=headers)
    elif x < 0.9 and not args.no_algo:
        body = {"trading_mode": rng.choice(["E", "F"]), "symbol": symbol, "fast_trading_price_limit": 1000 if rng.random() < 0.5 else 0.5,
                "fast_trading_account_ids": [account], "fast_trading_book_levels": 1, "fast_trading_target_percentage": 10,
                "fast_trading_cycle_seconds": 5, "fast_trading_test_mode": True}
        await timed(session, stats, "post:algo_start", "POST", args.base + "/api/submit-algo", json=body, headers=headers)
    elif x < 0.95 and not args.no_algo:
        body = {"account_ids": [account], "symbol": symbol, "quantity": 100, "mode": "urgent", "deadline": None}
        await timed(session, stats, "post:smart_start", "POST", args.base + "/api/smart-selling", json=body, headers=headers)
    elif not args.no_algo:
        if args.legacy:
            await timed(session, stats, "post:algo_stop", "POST", args.base + "/submit-algo-stop", data={"reason": "stress"}, headers={"Origin": args.base}, allow_redirects=False)
        else:
            await timed(session, stats, "post:algo_stop", "POST", args.base + "/api/algo/stop", json={"reason": "stress"}, headers=headers)


def proc_stat(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        ticks = int(fields[11]) + int(fields[12])
        threads = int(fields[17])
        with open(f"/proc/{pid}/statm") as f:
            rss = int(f.read().split()[1]) * 4096
        fds = len(os.listdir(f"/proc/{pid}/fd"))
        return ticks, rss, threads, fds
    except Exception:  # noqa: BLE001
        return 0, 0, 0, 0


async def monitor(args, stats, stop_at, cookie):
    hz = os.sysconf("SC_CLK_TCK")
    pids = [int(p) for p in subprocess.run(["pgrep", "-f", args.process], capture_output=True, text=True).stdout.split() if int(p) != os.getpid()]
    last_ticks, last_t = sum(proc_stat(p)[0] for p in pids), time.time()
    async with aiohttp.ClientSession(cookies={"auth_token": cookie}, cookie_jar=aiohttp.CookieJar(unsafe=True)) as session:
        n = 0
        while time.perf_counter() < stop_at:
            await asyncio.sleep(2)
            n += 1
            ticks, rss, threads, fds = 0, 0, 0, 0
            for p in pids:
                t, r, th, fd = proc_stat(p)
                ticks += t; rss += r; threads += th; fds += fd
            now = time.time()
            cpu = 100.0 * (ticks - last_ticks) / hz / max(now - last_t, 1e-9)
            last_ticks, last_t = ticks, now
            stats.samples.append({"t": round(now, 1), "cpu": round(cpu, 1), "rss_mb": round(rss / 1048576, 1), "threads": threads, "fds": fds})
            try:
                async with session.get(args.base + "/api/accounts", timeout=aiohttp.ClientTimeout(total=5)) as r:
                    payload = await r.json()
                newest = max((parse_iso(a["ts"]) for a in payload.get("accounts", []) if a.get("ts")), default=None)
                if newest is not None:
                    stats.staleness.append(round(time.time() - newest, 2))
            except Exception as exc:  # noqa: BLE001
                stats.errors.setdefault("monitor", []).append(str(exc)[:80])
            if not args.legacy:
                try:
                    async with session.get(args.base + "/api/health", timeout=aiohttp.ClientTimeout(total=5)) as r:
                        h = await r.json()
                    feeds = {k: v.get("state") for k, v in h.get("feeds", {}).items() if isinstance(v, dict)}
                    stats.health.append({"feeds": feeds, "live_connections": h.get("live_connections"),
                                         "commands": h.get("commands", {}).get("published"), "failed": h.get("commands", {}).get("failed")})
                except Exception as exc:  # noqa: BLE001
                    stats.errors.setdefault("monitor", []).append(str(exc)[:80])
            if args.kafka_bin and n % 3 == 0:
                lag = 0
                for group in args.groups.split(","):
                    out = await asyncio.get_running_loop().run_in_executor(None, lambda g=group: subprocess.run(
                        [args.kafka_bin + "/kafka-consumer-groups.sh", "--bootstrap-server", "127.0.0.1:9092", "--describe", "--group", g],
                        capture_output=True, text=True).stdout)
                    for line in out.splitlines():
                        parts = line.split()
                        if len(parts) >= 6 and parts[0] == group and parts[5].isdigit():
                            lag += int(parts[5])
                stats.lag.append(lag)


async def burst(args, start_after, duration):
    await asyncio.sleep(start_after)
    procs = [await asyncio.create_subprocess_exec(args.python, "-I", args.feeder, "--books-hz", "200", "--accounts-hz", "20",
                                                  "--quotes-hz", "20", "--volume-hz", "20", "--duration", str(duration),
                                                  stdout=asyncio.subprocess.DEVNULL) for _ in range(args.burst_feeders)]
    await asyncio.gather(*(p.wait() for p in procs))


async def main_async(args):
    stats = Stats()
    stop_at = time.perf_counter() + args.duration
    async with aiohttp.ClientSession(cookie_jar=aiohttp.CookieJar(unsafe=True)) as s:
        async with s.post(args.base + "/login", data={"username": "trader", "password": args.password}, allow_redirects=False) as r:
            cookie = r.cookies["auth_token"].value
    tasks = [asyncio.create_task(user(i, args, stats, stop_at)) for i in range(args.users)]
    tasks.append(asyncio.create_task(monitor(args, stats, stop_at, cookie)))
    if args.burst:
        tasks.append(asyncio.create_task(burst(args, args.duration / 3, args.duration / 3)))
    await asyncio.gather(*tasks, return_exceptions=True)
    return stats


def report(args, stats):
    print(f"\n== {args.users} sessions for {args.duration}s against {args.base}{' (legacy mode)' if args.legacy else ''}{' + data burst' if args.burst else ''}")
    print(f"{'request':22s} {'n':>6s} {'p50':>8s} {'p90':>8s} {'p99':>8s} {'max':>8s} {'rejected':>8s} {'errors':>6s}")
    total = 0
    for label in sorted(stats.lat):
        v = stats.lat[label]
        total += len(v)
        print(f"{label:22s} {len(v):6d} {pct(v, .5)*1000:7.1f}ms {pct(v, .9)*1000:7.1f}ms {pct(v, .99)*1000:7.1f}ms {max(v)*1000:7.1f}ms "
              f"{stats.rejected.get(label, 0):8d} {len(stats.errors.get(label, [])):6d}")
    s = stats.sse
    per_stream = s["events"] / max(s["opened"] - s["failed"], 1) / args.duration
    print(f"streams: opened={s['opened']} failed={s['failed']} closed_early={s['closed_early']} events={s['events']} "
          f"({per_stream:.2f}/stream/s) bytes={s['bytes']/1048576:.1f}MB max_gap={s['max_gap']:.1f}s gaps>5s={s['gaps_over_5s']}")
    if stats.samples:
        cpu = [x["cpu"] for x in stats.samples]
        print(f"server: cpu avg={statistics.mean(cpu):.0f}% max={max(cpu):.0f}%  rss max={max(x['rss_mb'] for x in stats.samples):.0f}MB "
              f"threads max={max(x['threads'] for x in stats.samples)} fds max={max(x['fds'] for x in stats.samples)}")
    if stats.staleness:
        print(f"served snapshot age: max={max(stats.staleness):.2f}s mean={statistics.mean(stats.staleness):.2f}s")
    if stats.lag:
        print(f"kafka consumer lag: max={max(stats.lag)} samples={len(stats.lag)}")
    if stats.health:
        states = {}
        for h in stats.health:
            for k, v in h["feeds"].items():
                states.setdefault(k, set()).add(v)
        print(f"feeds seen: { {k: sorted(v) for k, v in states.items()} } live_connections max={max(h['live_connections'] or 0 for h in stats.health)} "
              f"commands published={stats.health[-1]['commands']} failed={stats.health[-1]['failed']}")
    for label, sample in sorted(stats.rejection_samples.items()):
        print(f"  rejected sample {label}: {sample}")
    errors = {k: v for k, v in stats.errors.items() if v}
    print(f"total requests={total} unexpected errors={sum(len(v) for v in errors.values())}")
    for k, v in errors.items():
        print(f"  {k}: {len(v)} e.g. {v[0]}")
    return {"params": vars(args), "latency": {k: {"n": len(v), "p50": pct(v, .5), "p90": pct(v, .9), "p99": pct(v, .99), "max": max(v)} for k, v in stats.lat.items()},
            "rejected": stats.rejected, "errors": {k: len(v) for k, v in errors.items()}, "sse": stats.sse, "samples": stats.samples,
            "staleness": stats.staleness, "lag": stats.lag, "health": stats.health}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://127.0.0.1:8765")
    ap.add_argument("--users", type=int, default=48)
    ap.add_argument("--duration", type=int, default=60)
    ap.add_argument("--password", default="trader-demo")
    ap.add_argument("--lmh-password", default="lmh-demo")
    ap.add_argument("--min-interval", type=float, default=1.0)
    ap.add_argument("--max-interval", type=float, default=3.0)
    ap.add_argument("--legacy", action="store_true", help="Python trading_ui endpoints only")
    ap.add_argument("--no-algo", action="store_true")
    ap.add_argument("--process", default="build/nextgen-gui --config")
    ap.add_argument("--kafka-bin", default="")
    ap.add_argument("--groups", default="account-dashboard-demo,market-data-dashboard-demo")
    ap.add_argument("--burst", action="store_true", help="run a market-data burst during the middle third")
    ap.add_argument("--burst-feeders", type=int, default=2)
    ap.add_argument("--feeder", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "feeder.py"))
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    stats = asyncio.run(main_async(args))
    result = report(args, stats)
    if args.out:
        with open(args.out, "w") as f:
            json.dump(result, f, indent=1)


if __name__ == "__main__":
    main()
