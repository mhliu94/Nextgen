#!/usr/bin/env python3
"""Publishes realistic KTrader market/account traffic to Kafka for demos and benchmarks.

Topics: account-details (one snapshot per account), price-books (20-level books),
stock-quotes (last trade with previous close, daily volume and last size), trading-volume
(cumulative volume tape) and order-status (order-status.v1 snapshots).

With --broker the feeder also acts as a simulated broker: it consumes trading-commands and
answers LIMIT_ORDER / LIMIT_ORDER_FOK / MARKET_ORDER / CANCEL_OPEN_ORDERS with order-status
snapshots (acknowledgement, partial and full fills at the limit price, cancellations), so the
algorithm modes and the order monitor can be exercised end to end without a real broker.
--orders-per-minute adds independent "manual" orders for the configured accounts.
"""
import argparse
import itertools
import json
import random
import time
from datetime import datetime, timezone

from confluent_kafka import Consumer, Producer

SYMBOLS = {"TSLA": 250.0, "NVDA": 182.0, "AEHL": 1.40, "BIYA": 3.90, "POM": 0.95}
ACCOUNTS = [("TIGER_MAIN", 1), ("TIGER_ALT", 2), ("FUTU_HK", 3), ("GIGA_EMU", 4), ("IBKR_WIN", 5)]


def iso(ts=None):
    return datetime.fromtimestamp(ts if ts is not None else time.time(), tz=timezone.utc).isoformat().replace("+00:00", "Z")


def tick_for(price):
    return 0.01 if price >= 1 else 0.0001


def offset_iso(ts=None):
    """datetime.isoformat() of an aware UTC time, as order-status publishers emit it."""
    return datetime.fromtimestamp(ts if ts is not None else time.time(), tz=timezone.utc).isoformat()


class SimulatedBroker:
    """Answers trading commands with order-status.v1 snapshots (one broker order per command)."""

    def __init__(self, producer, mid, accounts, fill_probability=0.85, holdings=None, reject_probability=0.02):
        self.producer = producer
        self.mid = mid
        self.holdings = holdings  # Feeder.holdings: fills move cash and positions
        self.num_ids = dict(accounts)
        self.fill_probability = fill_probability
        self.reject_probability = reject_probability
        self.ids = itertools.count(1)
        self.run = format(int(time.time()), "x")  # broker order ids stay unique across feeder restarts
        self.orders = {}  # broker order id -> order state
        self.pending = []  # (due, broker order id, action)
        self.sent = 0

    def publish(self, order, status):
        order["sequence"] += 1
        order["status"] = status
        now = time.time()
        payload = {
            "schema_version": 1, "account_id": order["account_id"], "order_id": order["order_id"],
            "sequence": order["sequence"], "created_at": offset_iso(order["created"]), "updated_at": offset_iso(max(now, order["created"])),
            "symbol": order["symbol"], "side": order["side"], "order_type": order["order_type"], "status": status,
            "quantity": order["quantity"], "filled_quantity": order["filled"],
            "average_fill_price": order["avg_price"] if order["filled"] else None, "currency": "USD",
            "time_in_force": order["tif"],
        }
        if order["account_id"] in self.num_ids:
            payload["account_num_id"] = self.num_ids[order["account_id"]]
        if order["order_type"] == "LIMIT":
            payload["limit_price"] = order["limit_price"]
        if order.get("command_id"):
            payload["command_id"] = order["command_id"]
        if order.get("client_order_id"):
            payload["client_order_id"] = order["client_order_id"]
        self.producer.produce("order-status", key=json.dumps([order["account_id"], order["order_id"]]), value=json.dumps(payload))
        self.sent += 1

    def new_order(self, account_id, symbol, side, quantity, order_type, limit_price, tif="DAY", command_id=None, client_order_id=None):
        order = {"order_id": f"SIM-{self.run}-{next(self.ids):06d}", "account_id": account_id, "symbol": symbol, "side": side,
                 "quantity": quantity, "order_type": order_type, "limit_price": limit_price, "tif": tif,
                 "filled": 0, "avg_price": None, "sequence": -1, "status": None, "created": time.time(),
                 "command_id": command_id, "client_order_id": client_order_id}
        self.orders[order["order_id"]] = order
        self.publish(order, "PENDING_NEW" if random.random() < 0.3 else "NEW")
        return order

    def fill(self, order, quantity, price):
        quantity = min(quantity, order["quantity"] - order["filled"])
        if quantity <= 0:
            return
        cost = (order["avg_price"] or 0) * order["filled"] + price * quantity
        order["filled"] += quantity
        order["avg_price"] = round(cost / order["filled"], 4)
        if self.holdings is not None:
            self.holdings.apply_fill(order["account_id"], order["symbol"], order["side"], quantity, price)
        self.publish(order, "FILLED" if order["filled"] >= order["quantity"] else "PARTIALLY_FILLED")

    def schedule(self, delay, order, action):
        self.pending.append((time.time() + delay, order["order_id"], action))

    def on_command(self, command):
        kind = command.get("type")
        account_id = str(command.get("account_id") or "")
        symbol = str(command.get("symbol") or "").upper()
        if kind in ("LIMIT_ORDER", "LIMIT_ORDER_FOK", "MARKET_ORDER") and account_id and symbol in self.mid:
            quantity = command.get("qty_shares")
            if not isinstance(quantity, (int, float)) or quantity <= 0:
                notional = command.get("notional_usd")
                quantity = int(notional / self.mid[symbol]) if isinstance(notional, (int, float)) and notional > 0 else 0
            if quantity <= 0:
                return
            side = command.get("side", "BUY")
            limit = command.get("limit_price") if kind != "MARKET_ORDER" else None
            order = self.new_order(account_id, symbol, side, quantity, "MARKET" if limit is None else "LIMIT", limit,
                                   "FOK" if kind == "LIMIT_ORDER_FOK" else command.get("time_in_force") or "DAY",
                                   command.get("command_id"), command.get("client_order_id"))
            if kind == "LIMIT_ORDER_FOK":
                if random.random() < self.fill_probability:
                    self.fill(order, quantity, limit)
                else:
                    self.publish(order, "CANCELED")
            elif kind == "MARKET_ORDER":
                self.schedule(0.3, order, "fill_all")
            else:
                self.schedule(random.uniform(0.4, 1.5), order, "work")
        elif kind == "CANCEL_OPEN_ORDERS" and account_id:
            for order in list(self.orders.values()):
                if order["account_id"] == account_id and (not symbol or order["symbol"] == symbol) \
                        and order["status"] in ("PENDING_NEW", "NEW", "PARTIALLY_FILLED"):
                    self.publish(order, "PENDING_CANCEL")
                    self.schedule(random.uniform(0.2, 0.8), order, "cancel")

    def step(self):
        now = time.time()
        due = [item for item in self.pending if item[0] <= now]
        self.pending = [item for item in self.pending if item[0] > now]
        for _, order_id, action in due:
            order = self.orders.get(order_id)
            if order is None or order["status"] in ("FILLED", "CANCELED", "REJECTED", "EXPIRED"):
                continue
            price = order["limit_price"] if order["limit_price"] is not None else round(self.mid[order["symbol"]], 4)
            if action == "fill_all":
                self.fill(order, order["quantity"] - order["filled"], price)
            elif action == "cancel":
                self.publish(order, "CANCELED")
            elif action == "work":
                if order["status"] == "PENDING_NEW":
                    self.publish(order, "NEW")
                roll = random.random()
                if roll < self.reject_probability:
                    self.publish(order, "REJECTED" if order["filled"] == 0 else "CANCELED")
                elif roll < self.fill_probability:
                    remaining = order["quantity"] - order["filled"]
                    part = remaining if random.random() < 0.6 or remaining < 200 else max(1, int(remaining * random.uniform(0.2, 0.7)))
                    self.fill(order, part, price)
                    if order["status"] == "PARTIALLY_FILLED":
                        self.schedule(random.uniform(0.5, 2.0), order, "work")
                else:
                    self.schedule(random.uniform(1.0, 3.0), order, "work")
        # Forget old terminal orders.
        if len(self.orders) > 5000:
            for order_id in [k for k, v in self.orders.items() if v["status"] in ("FILLED", "CANCELED", "REJECTED")][:1000]:
                self.orders.pop(order_id, None)

    def manual_order(self):
        account_id = random.choice(list(self.num_ids))
        symbol = random.choice(list(self.mid))
        side = random.choice(["BUY", "SELL"])
        tick = tick_for(self.mid[symbol])
        limit = round(self.mid[symbol] + (-1 if side == "BUY" else 1) * tick * random.randint(0, 5), 4)
        order = self.new_order(account_id, symbol, side, random.choice([100, 200, 500, 1000, 2500]), "LIMIT", limit,
                               command_id=f"manual_{iso()}_{random.randint(1000, 9999)}")
        self.schedule(random.uniform(0.5, 4.0), order, "work")


class Holdings:
    """Per-account cash and positions that persist between snapshots and follow simulated fills."""

    def __init__(self, mid, disabled_probability):
        self.disabled_probability = disabled_probability
        self.accounts = {}
        for account_id, _ in ACCOUNTS:
            cash = round(random.uniform(50000, 500000), 2)
            positions = {}
            for symbol, px in mid.items():
                if random.random() < 0.85:
                    positions[symbol] = {"qty": random.choice([300, 500, 1200, 5000, 20000]),
                                         "avg_price": round(px * random.uniform(0.9, 1.1), 4)}
            self.accounts[account_id] = {"cash": cash, "hkd": round(random.uniform(0, 100000), 2), "positions": positions}

    def apply_fill(self, account_id, symbol, side, quantity, price):
        account = self.accounts.get(account_id)
        if account is None:
            return
        position = account["positions"].setdefault(symbol, {"qty": 0, "avg_price": price})
        if side == "BUY":
            total = position["qty"] + quantity
            position["avg_price"] = round((position["avg_price"] * position["qty"] + price * quantity) / total, 4) if total else price
            position["qty"] = total
            account["cash"] = round(account["cash"] - quantity * price, 2)
        else:
            position["qty"] = max(0, position["qty"] - quantity)
            account["cash"] = round(account["cash"] + quantity * price, 2)

    def snapshot(self, account_id, num_id):
        account = self.accounts[account_id]
        positions = [{"symbol": symbol, "qty": p["qty"], "avg_price": p["avg_price"], "available_qty": p["qty"]}
                     for symbol, p in account["positions"].items() if p["qty"] > 0]
        cash = account["cash"]
        return {"account_id": account_id, "account_num_id": num_id, "cash": cash,
                "cash_by_currency": {"USD": cash, "HKD": account["hkd"]},
                "available_cash_by_currency": {"USD": cash},
                "positions": positions, "ts": iso(), "trading_enabled": random.random() >= self.disabled_probability}


class Feeder:
    def __init__(self, bootstrap, levels, book_bytes_padding=0, state_file=None, disabled_probability=0.1):
        self.producer = Producer({"bootstrap.servers": bootstrap, "linger.ms": 2, "batch.num.messages": 1000,
                                  "queue.buffering.max.messages": 500000, "acks": "1"})
        self.levels = levels
        # Sequences continue across feeder restarts (consumers treat lower sequence numbers as stale).
        base = int(time.time() * 10)
        # A restarted feeder starts a new cumulative counter: announce it as a new feed, never as a
        # downward correction of the previous one.
        self.feed_id = f"feeder-{base}"
        self.sequence = {s: base for s in SYMBOLS}
        self.volume_sequence = {s: base for s in SYMBOLS}  # the volume tape has its own gap-free sequence
        self.last_volume_at = {}
        self.mid = dict(SYMBOLS)
        self.prev_close = {s: round(px * random.uniform(0.95, 1.05), 4) for s, px in SYMBOLS.items()}
        # Daily cumulative volume never decreases within a session, even across feeder restarts:
        # consumers treat a lower total as an unknown correction. Continue from the saved state.
        self.state_file = state_file
        self.session = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        self.volume = {s: random.randint(100000, 2000000) for s in SYMBOLS}
        if state_file:
            try:
                with open(state_file) as fh:
                    saved = json.load(fh)
                if saved.get("session") == self.session:
                    for symbol, total in saved.get("volume", {}).items():
                        if symbol in self.volume:
                            self.volume[symbol] = int(total) + 1
                    self.mid.update({k: v for k, v in saved.get("mid", {}).items() if k in self.mid})
                    self.prev_close.update({k: v for k, v in saved.get("prev_close", {}).items() if k in self.prev_close})
            except (OSError, ValueError):
                pass
        self.holdings = Holdings(self.mid, disabled_probability)
        self.sent = 0
        self.padding = book_bytes_padding

    def flush_poll(self):
        self.producer.poll(0)

    def save_state(self):
        if not self.state_file:
            return
        try:
            with open(self.state_file + ".tmp", "w") as fh:
                json.dump({"session": self.session, "volume": self.volume, "mid": self.mid, "prev_close": self.prev_close}, fh)
            import os
            os.replace(self.state_file + ".tmp", self.state_file)
        except OSError:
            pass

    def publish_accounts(self):
        for account_id, num_id in ACCOUNTS:
            payload = self.holdings.snapshot(account_id, num_id)
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
                       "trade_time": iso(time.time() - random.uniform(0, 2)), "observed_at": iso(), "is_delayed": False,
                       "is_halted": False, "healthy": True, "prev_close": str(self.prev_close[symbol]),
                       "volume": self.volume[symbol], "last_size": random.choice([1, 10, 25, 100, 100, 200, 500, 1200])}
            self.producer.produce("stock-quotes", key=symbol, value=json.dumps(payload))
            self.sent += 1

    def publish_volume(self):
        now = time.time()
        for symbol in SYMBOLS:
            self.volume[symbol] += random.randint(0, 2000)
            self.volume_sequence[symbol] += 1
            # Each observation covers the interval since the previous one.
            interval_start = self.last_volume_at.get(symbol, now - 1)
            self.last_volume_at[symbol] = now
            payload = {"schema_version": 1, "type": "TRADING_VOLUME", "symbol": symbol, "source": "TradeStation", "feed_id": self.feed_id,
                       "sequence": self.volume_sequence[symbol], "total_volume": self.volume[symbol], "observed_at": iso(now),
                       "interval_start": iso(interval_start), "session": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
                       "healthy": True, "is_halted": False, "is_delayed": False}
            self.producer.produce("trading-volume", key=symbol, value=json.dumps(payload))
            self.sent += 1
        self.save_state()


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
    ap.add_argument("--broker", action="store_true", help="answer trading-commands with order-status snapshots")
    ap.add_argument("--orders-per-minute", type=float, default=0, help="independent manual orders published to order-status")
    ap.add_argument("--fill-probability", type=float, default=0.85)
    ap.add_argument("--reject-probability", type=float, default=0.02, help="chance that a working order is rejected")
    ap.add_argument("--state-file", help="keep cumulative volume monotone across restarts (JSON file)")
    ap.add_argument("--disabled-probability", type=float, default=0.1,
                    help="chance that an account snapshot reports trading disabled")
    args = ap.parse_args()

    feeder = Feeder(args.bootstrap, args.levels, args.pad, args.state_file, args.disabled_probability)
    broker = None
    commands = None
    if args.broker or args.orders_per_minute:
        broker = SimulatedBroker(feeder.producer, feeder.mid, ACCOUNTS, args.fill_probability, feeder.holdings,
                                 args.reject_probability)
    if args.broker:
        commands = Consumer({"bootstrap.servers": args.bootstrap, "group.id": f"simulated-broker-{random.randint(0, 1 << 30)}",
                             "auto.offset.reset": "latest", "enable.auto.commit": False})
        commands.subscribe(["trading-commands"])
    next_manual = time.time()
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
            if commands is not None:
                message = commands.poll(0)
                while message is not None:
                    if not message.error() and message.value():
                        try:
                            broker.on_command(json.loads(message.value()))
                        except (ValueError, TypeError):
                            pass
                    message = commands.poll(0)
            if broker is not None:
                if args.orders_per_minute and now >= next_manual:
                    broker.manual_order(); next_manual += 60.0 / args.orders_per_minute
                broker.step()
            feeder.flush_poll()
            if now >= report:
                print(f"{iso(now)} sent={feeder.sent} rate={feeder.sent / (now - start):.0f}/s"
                      + (f" order_status={broker.sent}" if broker else ""), flush=True)
                report += 5
            if not did:
                time.sleep(0.002 if broker is not None else min(0.002, max(0.0, min(next_books, next_accounts, next_quotes, next_volume) - now)))
    except KeyboardInterrupt:
        pass
    if commands is not None:
        commands.close()
    feeder.producer.flush(10)
    elapsed = time.time() - start
    print(f"done sent={feeder.sent} elapsed={elapsed:.1f}s rate={feeder.sent / elapsed:.0f}/s", flush=True)


if __name__ == "__main__":
    main()
