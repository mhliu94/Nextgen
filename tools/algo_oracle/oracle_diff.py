#!/usr/bin/env python3
"""Differential test of the algorithm modes: C++ port vs the original Python implementation.

Generates random (seeded) scenarios for the Smart selling engine / market adapter / feedback
correlator and for the fast-trading cycle planner, runs each through both implementations and
compares the traces step by step. Any divergence is written to --out-dir for inspection.

    python tools/algo_oracle/oracle_diff.py --cpp-driver build/nextgen-algo-driver \
        --python /path/to/venv/bin/python --repo /path/to/KTrader --count 300 --seed 1
"""
import argparse
import json
import math
import os
import random
import subprocess
import sys
from decimal import Decimal, InvalidOperation

HERE = os.path.dirname(os.path.abspath(__file__))
EPOCH = 1800000000.0


# ----------------------------------------------------------------------------------------------
# scenario generators
# ----------------------------------------------------------------------------------------------

def iso(ts):
    from datetime import datetime, timezone
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()


def dec(value, places=2):
    return f"{value:.{places}f}"


def displayed_levels(rng, best, direction, tick=0.01):
    """Distinct displayed prices away from `best` (direction +1 for asks, -1 for bids), with gaps and repeats."""
    count = rng.choice([0, 1, 2, 4, 5, 6, 6, 8])
    prices, price = [], best
    for _ in range(count):
        prices.append(dec(price))
        price = max(tick, price + direction * tick * rng.choice([1, 1, 2, 5, 10, 40]))
    if prices and rng.random() < 0.3:
        prices += [prices[0]] * rng.randint(1, 3)  # repeated prices count once
    if rng.random() < 0.3:
        rng.shuffle(prices)
    return prices


def gen_smart(rng, index):
    buy = rng.random() < 0.3
    accounts = rng.choice([["A"], ["A"], ["A", "B"], ["A", "B", "C"]])
    step = rng.choice(["1", "1", "1", ".1"]) if not buy else rng.choice(["1", "1", "5"])
    modes = ["patient", "normal", "urgent", "1", "2", "3", "ultra", "4"]
    settings = {"symbol": "XYZ", "mode": rng.choice(modes),
                "tick_size": ".01", "quantity_step": step,
                "min_quantity": rng.choice([step, "100", "100", "100", "200"]),
                "min_notional": rng.choice(["0", "0", "100", "500"])}
    if rng.random() < 0.3:
        settings["deadline"] = None
    else:
        settings["horizon_seconds"] = rng.choice([30, 60, 120, 300, 600, 7200])
    quantities = {a: str(rng.choice([40, 100, 300, 500, 1000, 2000, 10000])) for a in accounts}
    if len(accounts) == 1:
        settings["account_id"] = accounts[0]
        settings["quantity"] = quantities[accounts[0]]
    else:
        settings["account_ids"] = accounts
        settings["account_quantities"] = quantities
        if rng.random() < 0.5:
            settings["quantity"] = str(sum(int(q) for q in quantities.values()))
        else:
            settings["quantity"] = str(rng.randint(10, sum(int(q) for q in quantities.values())))
    if rng.random() < 0.2:
        settings["participation_percentage"] = rng.choice(["1", "2.5", "12.25", "100"])
    bid = 10.0
    bound_key = "maximum_price" if buy else "minimum_price"
    if rng.random() < 0.35:
        # Bounds at, inside and several displayed levels away from the market.
        settings[bound_key] = rng.choice(["9.5", "9.9", "10", "10.005", "10.05", "10.5", "11.6", "8.5"])
    if rng.random() < 0.1:
        settings["imminent_event"] = True
    if rng.random() < 0.3:
        settings["disable_child_cap"] = rng.choice([True, True, False, "true", None, 1])
    if rng.random() < 0.35:
        settings["account_average_costs"] = {a: rng.choice([None, "9.5", "10", "10.02", "11.25", "12", 13]) for a in accounts
                                              if rng.random() < 0.85}
    if buy or rng.random() < 0.05:
        settings["account_budgets"] = {a: str(rng.choice([0, 500, 2500, 10000, 100000, 1e7])) for a in accounts}
        if rng.random() < 0.05:
            settings["account_budgets"].pop(accounts[0], None)

    use_market = rng.random() < 0.35
    steps = [{"op": "now", "t": 0.0}, {"op": "start", "settings": settings}]
    if buy:
        steps.append({"op": "cash", "limits": {a: rng.choice(["0", "1500", "5000", "250000"]) for a in accounts}})
    now = 0.0
    sequence = 0
    book_seq = 0
    cumulative = rng.randint(1000, 100000)
    trade_n = 0
    trades = []
    known_orders = 0  # upper bound guess for order indices
    order_sequences = {}
    length = rng.randint(15, 60)
    side = "BUY" if buy else "SELL"
    for _ in range(length):
        r = rng.random()
        if r < 0.22:
            now += rng.choice([0.2, 0.5, 1.0, 1.0, 2.0, 3.0, 5.0, 6.0, 10.0, 31.0])
            steps.append({"op": "now", "t": round(now, 3)})
        elif r < 0.42:
            bid = max(1.0, bid + rng.choice([-0.02, -0.01, 0, 0, 0, 0.01, 0.02]))
            spread = rng.choice([0.01, 0.01, 0.02, 0.05])
            if use_market:
                book_seq += 1
                bids = [{"prc": dec(bid), "qty": str(rng.choice([100, 400, 800, 1000, 5000]))}]
                offers = [{"prc": dec(bid + spread), "qty": str(rng.choice([99, 150, 1000, 5000]))}]
                for price in displayed_levels(rng, bid - 0.01, -1):
                    bids.append({rng.choice(["prc", "price"]): price, rng.choice(["qty", "quantity"]): rng.choice([0, 100, 700, "bad"])})
                for price in displayed_levels(rng, bid + spread + 0.01, 1):
                    offers.append({rng.choice(["prc", "price"]): price, rng.choice(["qty", "quantity"]): rng.choice([0, 100, 700, "bad"])})
                payload = {"symbol": "XYZ", "update_time": int((EPOCH + now) * 1e9), "sequence_num": book_seq,
                           "bid_side": {"levels": bids}, "offer_side": {"levels": offers}}
                if rng.random() < 0.1:
                    payload["kind"] = "PRICE_BOOK_HEARTBEAT"
                steps.append({"op": "price_book", "payload": payload})
            else:
                q = {"op": "quote", "bid": dec(bid), "ask": dec(bid + spread), "bid_size": str(rng.choice([9, 100, 400, 800, 1000, 5000]))}
                if buy or rng.random() < 0.5:
                    q["ask_size"] = rng.choice(["0", "99", "150", "1000", "5000", 400, None])
                if rng.random() < 0.6:
                    q["bid_prices"] = displayed_levels(rng, bid, -1)
                    q["ask_prices"] = displayed_levels(rng, bid + spread, 1)
                if rng.random() < 0.5:
                    sequence += rng.choice([1, 1, 1, 2])
                    q["sequence"] = sequence
                if rng.random() < 0.08:
                    q["healthy"] = False
                if rng.random() < 0.05:
                    q["reset_sequence"] = True
                if rng.random() < 0.05:
                    q["preserve_consumption"] = True
                if rng.random() < 0.05:
                    q["source_time"] = EPOCH + now - rng.choice([11, 20])
                steps.append(q)
        elif r < 0.58:
            if use_market:
                sequence += rng.choice([1, 1, 1, 1, 2])
                cumulative += rng.choice([0, 0, 100, 500, 1000, 2000, 10000])
                payload = {"schema_version": 1, "type": "TRADING_VOLUME", "symbol": "XYZ", "source": "test", "feed_id": "feed-a",
                           "sequence": sequence, "total_volume": cumulative, "observed_at": iso(EPOCH + now),
                           "interval_start": iso(EPOCH + now - 1), "session": "2027-01-15", "healthy": True,
                           "is_halted": False, "is_delayed": False}
                x = rng.random()
                if x < 0.05:
                    payload["is_halted"] = True
                    payload["healthy"] = False
                    payload["total_volume"] = None
                elif x < 0.08:
                    payload["observed_at"] = iso(EPOCH + now - 15)
                elif x < 0.10:
                    cumulative -= 50
                    payload["total_volume"] = cumulative
                steps.append({"op": "volume", "payload": payload})
            else:
                trade_n += 1
                t = {"op": "trade", "quantity": str(rng.choice([50, 100, 333, 500, 1000, 2000, 10000])), "event_id": f"t{trade_n}"}
                if rng.random() < 0.08 and trades:
                    t["correction_of"] = rng.choice(trades)
                    t["quantity"] = str(-rng.choice([10, 50, 100]))
                elif rng.random() < 0.05 and trades:
                    t["event_id"] = rng.choice(trades)  # duplicate
                if rng.random() < 0.4:
                    sequence += rng.choice([1, 1, 1, 2])
                    t["sequence"] = sequence
                if rng.random() < 0.05:
                    t["interval_start"] = EPOCH - 5
                trades.append(t["event_id"])
                steps.append(t)
        elif r < 0.72:
            steps.append({"op": "tick"})
            known_orders += 1
        elif r < 0.77:
            steps.append({"op": "drain"})
        elif r < 0.88:
            k = rng.randint(0, max(0, known_orders - 1))
            status = rng.choice(["NEW", "NEW", "PARTIALLY_FILLED", "PARTIALLY_FILLED", "FILLED", "CANCELED", "CANCELED", "REJECTED",
                                 "NOT_FOUND", "UNKNOWN", "PENDING_CANCEL"])
            seq = order_sequences.get(k, 0) + rng.choice([1, 1, 1, 0, 2])
            order_sequences[k] = seq
            if status in ("FILLED",):
                cumulative_value = f"$order_qty:{k}" if rng.random() < 0.8 else f"$order_part:{k}:0.4"
            elif status in ("PARTIALLY_FILLED", "CANCELED", "PENDING_CANCEL"):
                cumulative_value = rng.choice([f"$order_part:{k}:0.4", f"$order_part:{k}:0.6", "0", f"$order_qty:{k}"])
            else:
                cumulative_value = "0"
            rep = {"op": "report", "order": k, "status": status, "cumulative": cumulative_value, "sequence": seq,
                   "broker_order_id": f"broker-{k}"}
            if status in ("FILLED", "CANCELED", "REJECTED"):
                rep["final"] = rng.random() < 0.85
            if rng.random() < 0.05:
                rep["authoritative"] = False
            steps.append(rep)
        elif r < 0.92:
            k = rng.randint(0, max(0, known_orders - 1))
            steps.append(rng.choice([{"op": "prepare", "order": k}, {"op": "prepare", "order": k},
                                     {"op": "abort", "order": k, "reason": "local check failed"},
                                     {"op": "hold", "order": k, "reason": "metadata conflict"},
                                     {"op": "action_error", "order": k, "action": rng.choice(["submit", "cancel"]), "error": "timeout"}]))
        elif r < 0.935:
            steps.append({"op": "feed_health", "healthy": rng.random() < 0.5, "reason": "test", "reset_sequence": rng.random() < 0.3})
        elif r < 0.95:
            steps.append({"op": "stop", "parent": 0})
        elif r < 0.96:
            steps.append({"op": "forecast", "parent": 0, "quantity": rng.choice([None, "1000", "123.456"])})
        elif r < 0.975:
            steps.append({"op": "costs", "parent": 0,
                          "costs": {a: rng.choice([None, "9.5", "10", "10.02", "11.25", "12.5", "-1"]) for a in accounts
                                    if rng.random() < 0.8}})
        elif r < 0.985:
            steps.append({"op": "restart"})
            if buy and rng.random() < 0.8:
                steps.append({"op": "cash", "limits": {a: rng.choice(["0", "1500", "5000", "250000"]) for a in accounts}})
        elif r < 0.99 and buy:
            steps.append({"op": "cash", "limits": {a: rng.choice(["0", "150", "1500", "5000", "250000"]) for a in accounts}})
        else:
            if use_market:
                steps.append({"op": rng.choice(["volume_connected", "connected"]), "connected": rng.random() < 0.5})
            else:
                k = rng.randint(0, max(0, known_orders - 1))
                payload = {"schema_version": 1, "account_id": accounts[0] if len(accounts) == 1 else rng.choice(accounts),
                           "order_id": f"broker-{k}", "sequence": order_sequences.get(k, 0) + 1, "created_at": iso(EPOCH),
                           "updated_at": iso(EPOCH + now), "symbol": "XYZ", "side": side if rng.random() < 0.95 else "SELL",
                           "order_type": "LIMIT",
                           "status": rng.choice(["NEW", "PARTIALLY_FILLED", "CANCELED", "FILLED"]),
                           "quantity": f"$order_qty:{k}", "filled_quantity": 0, "average_fill_price": None,
                           "limit_price": f"$order_px:{k}", "command_id": f"$order_id:{k}"}
                order_sequences[k] = payload["sequence"]
                steps.append({"op": "snapshot", "payload": payload})
    steps.append({"op": "status"})
    return {"kind": "smart", "engine": "buy" if buy else "sell", "name": f"smart-{index}", "steps": steps}


def gen_flow(rng, index):
    """Mostly healthy sessions that submit, dispatch, fill and rotate children: the paths that matter most."""
    buy = rng.random() < 0.4
    accounts = rng.choice([["A"], ["A", "B"], ["A", "B", "C"], ["A", "B", "C", "D"]])
    step = rng.choice(["1", "1", "1", "5"]) if buy else rng.choice(["1", "1", "1", ".1"])
    modes = ["patient", "normal", "urgent"] + ([] if buy else ["ultra", "4"])
    settings = {"symbol": "XYZ", "mode": rng.choice(modes), "tick_size": ".01", "quantity_step": step,
                "min_quantity": rng.choice(["100", "100", "100", "200", "1"] if not buy else ["100", "100", "200"]),
                "min_notional": rng.choice(["0", "0", "0", "1500"])}
    if rng.random() < 0.4:
        settings["deadline"] = None
    else:
        settings["horizon_seconds"] = rng.choice([60, 300, 600, 7200])
    quantities = {a: str(rng.choice([150, 400, 400, 1000, 2000, 5000, 10000])) for a in accounts}
    if len(accounts) == 1:
        settings["account_id"] = accounts[0]
        settings["quantity"] = quantities[accounts[0]]
    else:
        settings["account_ids"] = accounts
        settings["account_quantities"] = quantities
        total = sum(int(q) for q in quantities.values())
        settings["quantity"] = str(total if rng.random() < 0.6 else rng.randint(100, total))
    bid = rng.choice([10.0, 10.0, 12.0, 20.0])
    if rng.random() < 0.3:
        offset = rng.choice([-0.5, -0.05, 0, 0.01, 0.05, 0.3, 1.5])
        settings["maximum_price" if buy else "minimum_price"] = dec(bid + offset)
    if rng.random() < 0.35:
        settings["disable_child_cap"] = rng.choice([True, True, False])
    if len(accounts) > 1 and rng.random() < 0.6:
        settings["account_average_costs"] = {a: rng.choice([None, dec(bid - 0.5), dec(bid), dec(bid + 0.1), dec(bid + 1), dec(bid + 3)])
                                              for a in accounts if rng.random() < 0.9}
    if buy:
        settings["account_budgets"] = {a: str(rng.choice([1500, 2500, 10000, 100000, 1e7])) for a in accounts}
    steps = [{"op": "now", "t": 0.0}, {"op": "start", "settings": settings}]
    if buy:
        steps.append({"op": "cash", "limits": {a: rng.choice(["2500", "5000", "250000", "250000"]) for a in accounts}})
    now = 0.0
    trade_n = 0
    sequence = 0

    def quote():
        nonlocal bid
        bid = max(1.0, bid + rng.choice([-0.01, 0, 0, 0, 0.01]))
        spread = rng.choice([0.01, 0.01, 0.02])
        q = {"op": "quote", "bid": dec(bid), "ask": dec(bid + spread),
             "bid_size": str(rng.choice([9, 400, 1000, 5000, 20000])),
             "ask_size": str(rng.choice([99, 150, 1000, 5000, 20000]))}
        if rng.random() < 0.5:
            q["bid_prices"] = displayed_levels(rng, bid, -1)
            q["ask_prices"] = displayed_levels(rng, bid + spread, 1)
        return q

    def trade():
        nonlocal trade_n, sequence
        trade_n += 1
        t = {"op": "trade", "quantity": str(rng.choice([333, 500, 1000, 1250, 2000, 5000, 20000, 100000])), "event_id": f"t{trade_n}"}
        if rng.random() < 0.5:
            sequence += 1
            t["sequence"] = sequence
        return t

    for _ in range(rng.randint(6, 18)):
        now += rng.choice([0.5, 1.0, 2.0, 5.0, 5.0, 6.0])
        steps.append({"op": "now", "t": round(now, 3)})
        steps.append(quote())
        steps.append(trade())
        steps.append({"op": "tick"})
        r = rng.random()
        if r < 0.85:
            if rng.random() < 0.25:
                steps.append(quote())  # the market moves before the handoff
            if buy and rng.random() < 0.2:
                steps.append({"op": "cash", "limits": {a: rng.choice(["0", "1500", "250000"]) for a in accounts}})
            steps.append({"op": "prepare_active"})
            path = rng.random()
            if path < 0.45:
                steps.append({"op": "report_active", "status": "NEW", "fraction": "0"})
                if rng.random() < 0.5:
                    steps.append({"op": "report_active", "status": "PARTIALLY_FILLED", "fraction": rng.choice(["0.4", "0.6"])})
                steps.append({"op": "report_active", "status": "FILLED", "fraction": "1", "final": True})
            elif path < 0.65:
                steps.append({"op": "report_active", "status": "PARTIALLY_FILLED", "fraction": "0.4"})
                now += rng.choice([2.0, 2.5, 6.0])
                steps.append({"op": "now", "t": round(now, 3)})
                steps.append(quote())
                steps.append({"op": "tick"})  # cancels the working child after two seconds
                steps.append({"op": "report_active", "status": "CANCELED", "fraction": rng.choice(["0.4", "0.6"]), "final": True})
            elif path < 0.8:
                steps.append({"op": "snapshot_active", "status": "NEW", "fraction": "0"})
                steps.append({"op": "snapshot_active", "status": rng.choice(["FILLED", "PARTIALLY_FILLED", "CANCELED", "REJECTED"]),
                              "fraction": rng.choice(["1", "0.5", "0"])})
                if rng.random() < 0.5:
                    steps.append({"op": "snapshot_active", "status": "FILLED", "fraction": "1"})
            elif path < 0.88:
                steps.append({"op": "report_active", "status": rng.choice(["REJECTED", "CANCELED"]), "fraction": "0", "final": True})
            elif path < 0.94:
                steps.append({"op": "report_active", "status": "UNKNOWN", "fraction": "0", "authoritative": False})
                steps.append({"op": "tick"})
                steps.append({"op": "report_active", "status": "FILLED", "fraction": "1", "final": True})
            # else: leave the child working
        elif r < 0.92:
            steps.append({"op": "abort_active", "reason": "local check failed"})
        extra = rng.random()
        if extra < 0.12 and len(accounts) > 1:
            steps.append({"op": "costs", "parent": 0,
                          "costs": {a: rng.choice([None, dec(bid - 1), dec(bid), dec(bid + 0.2), dec(bid + 2)]) for a in accounts}})
        elif extra < 0.16:
            steps.append({"op": "restart"})
            if buy:
                steps.append({"op": "cash", "limits": {a: "250000" for a in accounts}})
        elif extra < 0.19:
            steps.append({"op": "forecast", "parent": 0, "quantity": rng.choice(["1000", "50000"])})
        elif extra < 0.21:
            steps.append({"op": "stop", "parent": 0})
        elif extra < 0.23:
            steps.append({"op": "drain"})
    steps.append({"op": "status"})
    return {"kind": "smart", "engine": "buy" if buy else "sell", "name": f"flow-{index}", "steps": steps}


def gen_fast(rng, index):
    cases = []
    for _ in range(rng.randint(3, 8)):
        mode = rng.choice(["E", "F"])
        accounts = rng.sample(["A", "B", "C", "D"], rng.randint(1, 4))
        metas = {a: {"num_id": i + 1, "broker": rng.choice(["Tiger", "Giga"]),
                     "trading_medium": rng.choice(["API", "WEB", "WINDOWS", "EMULATOR"])} for i, a in enumerate(accounts)}
        base = rng.choice([1.0, 10.0, 100.0])
        tick = base / 100
        asks = [[round(base + i * tick, 4), rng.choice([100, 300, 500, 1000, 5000])] for i in range(rng.randint(0, 6))]
        bids = [[round(base - (i + 1) * tick, 4), rng.choice([100, 300, 500, 1000, 5000])] for i in range(rng.randint(0, 6))]
        case = {"trading_mode": mode, "symbol": "XYZ", "book": {"asks": asks, "bids": bids},
                "price_limit": round(base + rng.choice([-2, -1, 0, 1, 2, 3]) * tick, 4),
                "account_ids": accounts if rng.random() < 0.9 else accounts + [accounts[0]],
                "book_levels": rng.randint(1, 5), "metas": metas,
                "target_percentage": rng.choice([10, 30, 50, 70, 100, 100, 120, 150, 110, 130]),
                "now": 1000.0, "minimum_cycle_seconds_by_medium": {"API": 3, "WEB": 30, "WINDOWS": 40, "EMULATOR": 40}}
        if rng.random() < 0.8:
            snapshots = {}
            for a in accounts:
                if rng.random() < 0.07:
                    continue  # a participant without a snapshot
                cash = rng.choice([0, 500, 5000, 100000, 1e7])
                positions = [[rng.choice(["XYZ", "XYZ", "US.XYZ", " xyz/USD "]), rng.choice([0, 50, 200, 200.25, 2000, 100000, -100]),
                              rng.choice([None, 0, 50, 2000])]]
                if rng.random() < 0.2:
                    positions.append(["XYZ", rng.choice([10, 300.5, -50]), None])
                if rng.random() < 0.3:
                    positions.append(["OTHER", 999999, None])
                snapshots[a] = {"cash": cash, "cash_by_currency": {"USD": cash}, "available_cash_by_currency": {"USD": cash * rng.choice([0.5, 1.0])},
                                "positions": positions, "trading_enabled": rng.random() < 0.9}
            case["snapshots"] = snapshots
        if rng.random() < 0.45:
            case["total_target_position"] = rng.choice([0, 1, 100, 200, 250, 400, 600, 2000, 4000, 100000, 300000, -1])
        if rng.random() < 0.6:
            case["last_action"] = {a: 1000.0 - rng.choice([1, 2.9, 3, 29, 30, 39, 40, 100]) for a in accounts if rng.random() < 0.7}
        if rng.random() < 0.1:
            case["price_limit"] = rng.choice([0, -1])
        cases.append(case)
    return {"kind": "fast", "name": f"fast-{index}", "cases": cases}


# ----------------------------------------------------------------------------------------------
# comparison
# ----------------------------------------------------------------------------------------------

def as_number(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return Decimal(str(value))
    if isinstance(value, str):
        try:
            return Decimal(value)
        except (InvalidOperation, ValueError):
            return None
    return None


def values_equal(a, b, path, diffs, lenient_messages):
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                diffs.append(f"{path}.{key}: missing on {'python' if key not in b else 'cpp'} side")
                continue
            if key in ("reason", "error") and lenient_messages:
                if (a[key] in (None, "")) != (b[key] in (None, "")):
                    diffs.append(f"{path}.{key}: {a[key]!r} vs {b[key]!r}")
                continue
            values_equal(a[key], b[key], f"{path}.{key}", diffs, lenient_messages)
        return
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            diffs.append(f"{path}: list length {len(a)} vs {len(b)}")
            return
        for i, (x, y) in enumerate(zip(a, b)):
            values_equal(x, y, f"{path}[{i}]", diffs, lenient_messages)
        return
    na, nb = as_number(a), as_number(b)
    if na is not None and nb is not None:
        if na == nb:
            return
        if (isinstance(a, float) or isinstance(b, float)) and math.isclose(float(na), float(nb), rel_tol=1e-9, abs_tol=1e-9):
            return
        # Diagnostic estimates (never used for a decision): Python keeps up to 80 significant digits of
        # a non-terminating quotient, the port truncates at 30 fractional digits. Agreement to 20
        # significant digits is treated as identical.
        if path.endswith(("volume_rate", "required_future_volume", "eligible_volume")) and na != 0 \
                and abs((na - nb) / na) < Decimal("1e-20"):
            return
        diffs.append(f"{path}: {a!r} vs {b!r}")
        return
    if a != b:
        diffs.append(f"{path}: {a!r} vs {b!r}")


def compare(cpp, py, lenient_messages):
    diffs = []
    if len(cpp) != len(py):
        diffs.append(f"trace length {len(cpp)} vs {len(py)}")
    for c, p in zip(cpp, py):
        label = f"step {c.get('step', c.get('case'))} ({c.get('op', 'case')})"
        ce, pe = c.get("error"), p.get("error")
        if (ce is None) != (pe is None):
            diffs.append(f"{label}: error {ce!r} vs {pe!r}")
        elif ce is not None and pe is not None and not lenient_messages and ce != pe:
            diffs.append(f"{label}: error text {ce!r} vs {pe!r}")
        values_equal(c.get("result"), p.get("result"), f"{label}.result", diffs, lenient_messages)
        values_equal(c.get("parents"), p.get("parents"), f"{label}.parents", diffs, lenient_messages)
        if diffs:
            break
    return diffs


def run_driver(cmd, scenario, env=None):
    proc = subprocess.run(cmd, input=json.dumps(scenario), capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed: {proc.stderr[-2000:]}")
    return json.loads(proc.stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp-driver", required=True)
    ap.add_argument("--python", required=True, help="interpreter with confluent_kafka etc. installed")
    ap.add_argument("--repo", required=True, help="KTrader checkout (PYTHONPATH for trading_ui)")
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--kind", choices=["smart", "fast", "both"], default="both")
    ap.add_argument("--scenario", help="run a single scenario file instead of generating")
    ap.add_argument("--out-dir", default="oracle-failures")
    ap.add_argument("--strict-messages", action="store_true", help="also compare reason/error texts")
    args = ap.parse_args()

    env = dict(os.environ)
    env["KTRADER_REPO"] = os.path.abspath(args.repo)
    py_cmd = [args.python, os.path.join(HERE, "py_driver.py")]
    cpp_cmd = [args.cpp_driver]

    scenarios = []
    if args.scenario:
        with open(args.scenario) as f:
            scenarios.append(json.load(f))
    else:
        rng = random.Random(args.seed)
        for i in range(args.count):
            if args.kind == "smart" or (args.kind == "both" and i % 3 != 2):
                # Half random stress (errors, gaps, halts), half productive order flow.
                scenarios.append(gen_flow(rng, i) if rng.random() < 0.5 else gen_smart(rng, i))
            else:
                scenarios.append(gen_fast(rng, i))

    os.makedirs(args.out_dir, exist_ok=True)
    failures = 0
    steps_total = 0
    for scenario in scenarios:
        cpp = run_driver(cpp_cmd, scenario)
        py = run_driver(py_cmd, scenario, env=env)
        steps_total += len(cpp)
        diffs = compare(cpp, py, not args.strict_messages)
        if diffs:
            failures += 1
            name = scenario.get("name", "scenario")
            with open(os.path.join(args.out_dir, f"{name}.json"), "w") as f:
                json.dump({"scenario": scenario, "cpp": cpp, "python": py, "diffs": diffs}, f, indent=1, default=str)
            print(f"DIVERGENCE {name}: {diffs[0]}" + (f" (+{len(diffs) - 1} more)" if len(diffs) > 1 else ""), flush=True)
    print(f"{len(scenarios)} scenarios, {steps_total} steps/cases compared, {failures} divergent "
          f"({'details in ' + args.out_dir if failures else 'all identical'})")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
