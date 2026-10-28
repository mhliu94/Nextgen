#!/usr/bin/env python3
"""Head-to-head benchmark: nextgen-gui (C++) vs KTrader trading_ui (Python/uvicorn).

Measures HTTP throughput/latency with wrk, SSE fan-out cost, Kafka ingestion lag under a burst,
and process CPU/RSS. Both servers must already be running against the same Kafka broker.
"""
import argparse
import json
import re
import subprocess
import sys
import threading
import time
import urllib.request
import http.client
from http.cookies import SimpleCookie

KAFKA_BIN = None


def login(base, user, password):
    data = f"username={user}&password={password}".encode()
    req = urllib.request.Request(base + "/login", data=data, method="POST",
                                 headers={"Content-Type": "application/x-www-form-urlencoded"})
    opener = urllib.request.build_opener(NoRedirect)
    try:
        resp = opener.open(req, timeout=10)
    except urllib.error.HTTPError as e:
        resp = e
    cookie = SimpleCookie()
    for header in resp.headers.get_all("Set-Cookie") or []:
        cookie.load(header)
    token = cookie.get("auth_token")
    if not token:
        raise SystemExit(f"login failed for {base}: status={resp.status} headers={dict(resp.headers)}")
    return token.value


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def pid_of(pattern):
    out = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True).stdout.split()
    pids = [int(p) for p in out if int(p) != __import__("os").getpid()]
    if not pids:
        raise SystemExit(f"no process matches {pattern!r}")
    return pids


def proc_stat(pids):
    """Sum of utime+stime ticks and RSS bytes across pids (uvicorn is a single process; include children)."""
    ticks = 0
    rss = 0
    for pid in pids:
        try:
            with open(f"/proc/{pid}/stat") as f:
                fields = f.read().rsplit(")", 1)[1].split()
            ticks += int(fields[11]) + int(fields[12])
            with open(f"/proc/{pid}/statm") as f:
                rss += int(f.read().split()[1]) * 4096
        except FileNotFoundError:
            pass
    return ticks, rss


class CpuSampler:
    def __init__(self, pids):
        self.pids = pids
        self.hz = __import__("os").sysconf("SC_CLK_TCK")

    def __enter__(self):
        self.t0 = time.time()
        self.ticks0, _ = proc_stat(self.pids)
        return self

    def __exit__(self, *exc):
        t1 = time.time()
        ticks1, rss = proc_stat(self.pids)
        self.cpu_percent = 100.0 * (ticks1 - self.ticks0) / self.hz / max(t1 - self.t0, 1e-9)
        self.rss_mb = rss / 1048576


def wrk(url, cookie, threads, connections, duration):
    cmd = ["wrk", f"-t{threads}", f"-c{connections}", f"-d{duration}s", "--latency", "-H", f"Cookie: auth_token={cookie}", url]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    result = {"raw": out}
    m = re.search(r"Requests/sec:\s+([\d.]+)", out)
    result["rps"] = float(m.group(1)) if m else 0.0
    for pct in ("50%", "90%", "99%"):
        m = re.search(re.escape(pct) + r"\s+([\d.]+)(us|ms|s)", out)
        if m:
            value, unit = float(m.group(1)), m.group(2)
            ms = value / 1000 if unit == "us" else value if unit == "ms" else value * 1000
            result["p" + pct.rstrip("%")] = ms
    m = re.search(r"Non-2xx or 3xx responses:\s+(\d+)", out)
    result["non2xx"] = int(m.group(1)) if m else 0
    m = re.search(r"Socket errors: connect (\d+), read (\d+), write (\d+), timeout (\d+)", out)
    result["errors"] = sum(int(x) for x in m.groups()) if m else 0
    m = re.search(r"(\d+) requests in", out)
    result["requests"] = int(m.group(1)) if m else 0
    return result


def http_get(base, path, cookie, timeout=30):
    req = urllib.request.Request(base + path, headers={"Cookie": f"auth_token={cookie}"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status, resp.read()


def sse_fanout(base, path, cookie, clients, duration):
    """Opens `clients` SSE connections and counts delivered events and bytes for `duration` seconds."""
    host, port = base.replace("http://", "").split(":")
    counts = {"events": 0, "bytes": 0, "connected": 0, "failed": 0}
    lock = threading.Lock()
    stop = time.time() + duration

    def worker():
        try:
            conn = http.client.HTTPConnection(host, int(port), timeout=10)
            conn.request("GET", path, headers={"Cookie": f"auth_token={cookie}", "Accept": "text/event-stream"})
            resp = conn.getresponse()
            if resp.status != 200:
                with lock:
                    counts["failed"] += 1
                return
            with lock:
                counts["connected"] += 1
            events = 0
            nbytes = 0
            sock = conn.sock
            sock.settimeout(1.0)
            while time.time() < stop:
                try:
                    chunk = sock.recv(65536)
                except TimeoutError:
                    continue
                except OSError:
                    break
                if not chunk:
                    break
                nbytes += len(chunk)
                events += chunk.count(b"\n\n")
            with lock:
                counts["events"] += events
                counts["bytes"] += nbytes
            conn.close()
        except Exception:
            with lock:
                counts["failed"] += 1

    threads = [threading.Thread(target=worker, daemon=True) for _ in range(clients)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(duration + 15)
    return counts


def parse_iso(ts):
    from datetime import datetime, timezone
    ts = ts.replace("Z", "+00:00")
    return datetime.fromisoformat(ts).timestamp()


def staleness_sampler(servers, stop_event, samples, interval=0.5):
    """Records, per server, how old the freshest served account snapshot is (seconds)."""
    while not stop_event.is_set():
        for name, s in servers.items():
            try:
                t0 = time.time()
                _, body = http_get(s["base"], "/api/accounts", s["cookie"], timeout=5)
                payload = json.loads(body)
                newest = max((parse_iso(a["ts"]) for a in payload.get("accounts", []) if a.get("ts")), default=None)
                if newest is not None:
                    samples.setdefault(name, []).append((round(t0, 3), round(t0 - newest, 3)))
            except Exception as exc:  # noqa: BLE001
                samples.setdefault(name + "_errors", []).append(str(exc)[:80])
        stop_event.wait(interval)


def consumer_lag(group):
    out = subprocess.run([KAFKA_BIN + "/kafka-consumer-groups.sh", "--bootstrap-server", "127.0.0.1:9092", "--describe", "--group", group],
                         capture_output=True, text=True).stdout
    lag = 0
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 6 and parts[0] == group and parts[5].isdigit():
            lag += int(parts[5])
    return lag, out


def main():
    global KAFKA_BIN
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp", default="http://127.0.0.1:8765")
    ap.add_argument("--py", default="http://127.0.0.1:8766")
    ap.add_argument("--user", default="trader")
    ap.add_argument("--password", default="trader-demo")
    ap.add_argument("--duration", type=int, default=15)
    ap.add_argument("--connections", type=int, default=64)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--sse-clients", type=int, default=200)
    ap.add_argument("--feeders", type=int, default=4, help="parallel feeder processes for the Kafka burst")
    ap.add_argument("--latency-connections", type=int, default=8, help="second, unsaturated HTTP pass (0 = skip)")
    ap.add_argument("--kafka-bin", required=True)
    ap.add_argument("--feeder", required=True)
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--out", default="bench-results.json")
    ap.add_argument("--skip", default="", help="comma list: http,sse,kafka")
    args = ap.parse_args()
    KAFKA_BIN = args.kafka_bin
    skip = set(filter(None, args.skip.split(",")))

    servers = {
        "cpp": {"base": args.cpp, "pids": pid_of("build/nextgen-gui"), "group": "account-dashboard-demo", "market_group": "market-data-dashboard-demo"},
        "py": {"base": args.py, "pids": pid_of("uvicorn trading_ui.webserver:app"), "group": "account-dashboard-py", "market_group": "market-data-dashboard-py"},
    }
    for name, s in servers.items():
        s["cookie"] = login(s["base"], args.user, args.password)
        _, idle_rss = proc_stat(s["pids"])
        s["idle_rss_mb"] = idle_rss / 1048576
        print(f"[{name}] pids={s['pids']} idle RSS={s['idle_rss_mb']:.1f} MB", flush=True)

    results = {"params": vars(args), "servers": {k: {"idle_rss_mb": v["idle_rss_mb"]} for k, v in servers.items()}, "http": {}, "sse": {}, "kafka": {}}

    # Warm-up: both servers must be serving real account snapshots and books before any measurement.
    for name, s in servers.items():
        deadline = time.time() + 60
        while time.time() < deadline:
            _, body = http_get(s["base"], "/api/accounts", s["cookie"])
            _, book = http_get(s["base"], "/api/market-insights?symbol=TSLA&depth=20", s["cookie"])
            if json.loads(body).get("accounts") and json.loads(book).get("bids"):
                break
            time.sleep(1)
        else:
            raise SystemExit(f"{name}: no account/book data after 60s")
        print(f"[{name}] warm: accounts={len(json.loads(body)['accounts'])} book levels={len(json.loads(book)['bids'])}", flush=True)

    endpoints = [
        ("accounts_json", "/api/accounts"),
        ("market_data_json", "/api/market-data?symbol=TSLA"),
        ("market_insights_json", "/api/market-insights?symbol=TSLA&depth=20"),
        ("account_details_page", "/account-details"),
        ("market_data_page", "/market-data"),
    ]
    if "http" not in skip:
        passes = [("throughput", args.connections, args.threads)]
        if args.latency_connections:
            passes.append(("latency", args.latency_connections, min(args.threads, args.latency_connections)))
        for pass_name, connections, threads in passes:
            results["http"][pass_name] = {}
            for label, path in endpoints:
                results["http"][pass_name][label] = {}
                for name, s in servers.items():
                    status, body = http_get(s["base"], path, s["cookie"])
                    with CpuSampler(s["pids"]) as cpu:
                        r = wrk(s["base"] + path, s["cookie"], threads, connections, args.duration)
                    r.update({"cpu_percent": cpu.cpu_percent, "rss_mb": cpu.rss_mb, "status": status, "body_bytes": len(body)})
                    raw = r.pop("raw")
                    results["http"][pass_name][label][name] = r
                    print(f"[{name}] {pass_name}/c{connections} {label:22s} {r['rps']:9.1f} req/s  p50={r.get('p50', 0):.2f}ms p90={r.get('p90', 0):.2f}ms p99={r.get('p99', 0):.2f}ms "
                          f"cpu={cpu.cpu_percent:.0f}% rss={cpu.rss_mb:.0f}MB non2xx={r['non2xx']} errors={r['errors']} body={len(body)}B", flush=True)
                    if r["non2xx"] or r["errors"]:
                        print(raw, flush=True)

    if "sse" not in skip:
        for name, s in servers.items():
            path = "/api/account-details/stream"
            with CpuSampler(s["pids"]) as cpu:
                counts = sse_fanout(s["base"], path, s["cookie"], args.sse_clients, args.duration)
            counts.update({"cpu_percent": cpu.cpu_percent, "rss_mb": cpu.rss_mb, "clients": args.sse_clients, "duration": args.duration})
            results["sse"][name] = counts
            print(f"[{name}] SSE x{args.sse_clients}: connected={counts['connected']} failed={counts['failed']} events={counts['events']} "
                  f"({counts['events'] / max(counts['connected'], 1) / args.duration:.2f}/client/s) bytes={counts['bytes'] / 1048576:.1f}MB "
                  f"cpu={cpu.cpu_percent:.0f}% rss={cpu.rss_mb:.0f}MB", flush=True)

    if "kafka" not in skip:
        # Burst: flood price-books/accounts for `duration` seconds, then measure how far behind each server is.
        for name, s in servers.items():
            s["t0"] = proc_stat(s["pids"])
        t0 = time.time()
        stop_event = threading.Event()
        staleness = {}
        sampler = threading.Thread(target=staleness_sampler, args=(servers, stop_event, staleness), daemon=True)
        sampler.start()
        procs = [subprocess.Popen([args.python, "-I", args.feeder, "--books-hz", "400", "--accounts-hz", "40", "--quotes-hz", "50",
                                   "--volume-hz", "50", "--duration", str(args.duration)], stdout=subprocess.PIPE, text=True)
                 for _ in range(args.feeders)]
        sent = 0
        for proc in procs:
            out, _ = proc.communicate()
            m = re.search(r"done sent=(\d+)", out)
            sent += int(m.group(1)) if m else 0
        elapsed = time.time() - t0
        time.sleep(3)  # keep sampling while the consumers drain
        stop_event.set()
        sampler.join(10)
        results["kafka"]["staleness"] = staleness
        for name in servers:
            values = [v for _, v in staleness.get(name, [])]
            if values:
                print(f"[{name}] served-snapshot age during burst: max={max(values):.2f}s mean={sum(values) / len(values):.2f}s samples={len(values)} errors={len(staleness.get(name + '_errors', []))}", flush=True)
        results["kafka"]["sent"] = sent
        results["kafka"]["feeder_rate"] = sent / elapsed
        print(f"[feeder] sent={sent} in {elapsed:.1f}s ({sent / elapsed:.0f} msg/s)", flush=True)
        hz = __import__("os").sysconf("SC_CLK_TCK")
        for name, s in servers.items():
            ticks1, rss = proc_stat(s["pids"])
            cpu = 100.0 * (ticks1 - s["t0"][0]) / hz / elapsed
            lags = {}
            # Give the consumers a few seconds to drain, sampling the lag as we go.
            for sample in range(4):
                lag_a, _ = consumer_lag(s["group"])
                lag_m, _ = consumer_lag(s["market_group"])
                lags[f"t+{sample * 2}s"] = {"account_details": lag_a, "market_data": lag_m}
                if lag_a == 0 and lag_m == 0:
                    break
                time.sleep(2)
            results["kafka"][name] = {"cpu_percent_during_burst": cpu, "rss_mb": rss / 1048576, "lag_samples": lags}
            print(f"[{name}] burst cpu={cpu:.0f}% rss={rss / 1048576:.0f}MB lag={json.dumps(lags)}", flush=True)

    with open(args.out, "w") as f:
        json.dump(results, f, indent=2)
    print(f"results written to {args.out}")


if __name__ == "__main__":
    main()
