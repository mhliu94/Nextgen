#!/usr/bin/env python3
"""Publishes realistic KTrader market/account traffic to Kafka for demos and benchmarks.

Topics: account-details (one snapshot per account), price-books (20-level books),
stock-quotes (last trade), trading-volume (cumulative volume tape).
"""
import argparse
import json
import random
import time
from datetime import datetime, timezone

from confluent_kafka import Producer

SYMBOLS = {"TSLA": 250.0, "NVDA": 182.0, "AEHL": 1.40, "BIYA": 3.90, "POM": 0.95}
ACCOUNTS = [("TIGER_MAIN", 1), ("TIGER_ALT", 2), ("FUTU_HK", 3), ("GIGA_EMU", 4), ("IBKR_WIN", 5)]


def iso(ts=None):
    return datetime.fromtimestamp(ts if ts is not None else time.time(), tz=timezone.utc).isoformat().replace("+00:00", "Z")


def tick_for(price):
    return 0.01 if price >= 1 else 0.0001


class Feeder:
    def __init__(self, bootstrap, levels, book_bytes_padding=0):
        self.producer = Producer({"bootstrap.servers": bootstrap, "linger.ms": 2, "batch.num.messages": 1000,
                                  "queue.buffering.max.messages": 500000, "acks": "1"})
        self.levels = levels
        self.sequence = {s: 0 for s in SYMBOLS}
        self.volume = {s: random.randint(100000, 2000000) for s in SYMBOLS}
        self.mid = dict(SYMBOLS)
        self.sent = 0
        self.padding = book_bytes_padding

    def flush_poll(self):
        self.producer.poll(0)

    def account_snapshot(self, account_id, num_id):
        positions = []
        for symbol, px in self.mid.items():
            if random.random() < 0.85:
                qty = random.choice([100, 300, 500, 1200, 5000, 20000])
                positions.append({"symbol": symbol, "qty": qty, "avg_price": round(px * random.uniform(0.9, 1.1), 2),
                                  "available_qty": qty})
        cash = round(random.uniform(5000, 500000), 2)
        return {"account_id": account_id, "account_num_id": num_id, "cash": cash,
                "cash_by_currency": {"USD": cash, "HKD": round(random.uniform(0, 100000), 2)},
                "available_cash_by_currency": {"USD": round(cash * random.uniform(0.6, 1.0), 2)},
                "positions": positions, "ts": iso(), "trading_enabled": random.random() > 0.1}

    def publish_accounts(self):
        for account_id, num_id in ACCOUNTS:
            payload = self.account_snapshot(account_id, num_id)
            self.producer.produce("account-details", key=account_id, value=json.dumps(payload))
            self.sent += 1

    def book(self, symbol):
        mid = self.mid[symbol]
        tick = tick_for(mid)
        mid = max(tick * 5, mid * (1 + random.gauss(0, 0.0004)))
        self.mid[symbol] = mid
        self.sequence[symbol] += 1
        bid0 = round(mid - tick / 2, 4)
        ask0 = round(mid + tick / 2, 4)
        bids = [{"prc": round(bid0 - i * tick, 4), "qty": random.randint(100, 5000)} for i in range(self.levels)]
        asks = [{"prc": round(ask0 + i * tick, 4), "qty": random.randint(100, 5000)} for i in range(self.levels)]
        now_ns = time.time_ns()
        payload = {"symbol": symbol, "update_time": now_ns, "sequence_num": self.sequence[symbol], "volume": self.volume[symbol],
                   "bid_side": {"levels": bids}, "offer_side": {"levels": asks}}
        if self.padding:
            payload["pad"] = "x" * self.padding
        return payload

    def publish_books(self):
        for symbol in SYMBOLS:
            self.producer.produce("price-books", key=symbol, value=json.dumps(self.book(symbol)))
            self.sent += 1

    def publish_quotes(self):
        for symbol, mid in self.mid.items():
            payload = {"schema_version": 1, "type": "STOCK_QUOTE", "symbol": symbol, "last": round(mid, 4),
                       "trade_time": iso(), "observed_at": iso(), "is_delayed": False, "is_halted": False, "healthy": True}
            self.producer.produce("stock-quotes", key=symbol, value=json.dumps(payload))
            self.sent += 1

    def publish_volume(self):
        now = time.time()
        for symbol in SYMBOLS:
            self.volume[symbol] += random.randint(0, 2000)
            payload = {"schema_version": 1, "type": "TRADING_VOLUME", "symbol": symbol, "source": "TradeStation", "feed_id": "feeder-1",
                       "sequence": self.sequence[symbol], "total_volume": self.volume[symbol], "observed_at": iso(now),
                       "interval_start": iso(now - 1), "session": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
                       "healthy": True, "is_halted": False, "is_delayed": False}
            self.producer.produce("trading-volume", key=symbol, value=json.dumps(payload))
            self.sent += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bootstrap", default="127.0.0.1:9092")
    ap.add_argument("--books-hz", type=float, default=2.0, help="price-book snapshots per second per symbol")
    ap.add_argument("--accounts-hz", type=float, default=0.5, help="account snapshots per second per account")
    ap.add_argument("--quotes-hz", type=float, default=1.0)
    ap.add_argument("--volume-hz", type=float, default=1.0)
    ap.add_argument("--levels", type=int, default=20)
    ap.add_argument("--duration", type=float, default=0, help="seconds (0 = run until interrupted)")
    ap.add_argument("--pad", type=int, default=0, help="extra bytes per book message")
    args = ap.parse_args()

    feeder = Feeder(args.bootstrap, args.levels, args.pad)
    start = time.time()
    next_books = next_accounts = next_quotes = next_volume = start
    report = start + 5
    try:
        while True:
            now = time.time()
            if args.duration and now - start >= args.duration:
                break
            did = False
            if args.books_hz and now >= next_books:
                feeder.publish_books(); next_books += 1.0 / args.books_hz; did = True
            if args.accounts_hz and now >= next_accounts:
                feeder.publish_accounts(); next_accounts += 1.0 / args.accounts_hz; did = True
            if args.quotes_hz and now >= next_quotes:
                feeder.publish_quotes(); next_quotes += 1.0 / args.quotes_hz; did = True
            if args.volume_hz and now >= next_volume:
                feeder.publish_volume(); next_volume += 1.0 / args.volume_hz; did = True
            feeder.flush_poll()
            if now >= report:
                print(f"{iso(now)} sent={feeder.sent} rate={feeder.sent / (now - start):.0f}/s", flush=True)
                report += 5
            if not did:
                time.sleep(min(0.002, max(0.0, min(next_books, next_accounts, next_quotes, next_volume) - now)))
    except KeyboardInterrupt:
        pass
    feeder.producer.flush(10)
    elapsed = time.time() - start
    print(f"done sent={feeder.sent} elapsed={elapsed:.1f}s rate={feeder.sent / elapsed:.0f}/s", flush=True)


if __name__ == "__main__":
    main()
