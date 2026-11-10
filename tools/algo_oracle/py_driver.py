#!/usr/bin/env python3
"""Runs an algo-oracle scenario against the original Python implementation (trading_ui).

Usage: PYTHONPATH=<KTrader checkout> python py_driver.py < scenario.json > trace.json

The trace format is identical to tools/algo_oracle/driver.cpp so that oracle_diff.py can
compare the two implementations step by step.
"""
import inspect
import json
import os
import shutil
import sys
import tempfile
from decimal import Decimal, ROUND_FLOOR

EPOCH = 1800000000.0

# The KTrader checkout that provides trading_ui (set by oracle_diff.py).
if os.environ.get("KTRADER_REPO"):
    sys.path.insert(0, os.environ["KTRADER_REPO"])

PARENT_KEYS = ["state", "order_state", "active", "mode", "participation", "quantity", "earned", "free_credit", "reserved",
               "filled", "discarded", "remaining", "fallback", "child_cap", "disable_child_cap", "reason", "start", "deadline",
               "residual_provisional", "fills_after_cancel", "stop_requested", "symbol", "account_id", "account_ids",
               "minimum_price", "maximum_price", "trading_mode", "capacity", "imminent_event", "time_in_force"]
ACCOUNT_KEYS = ["quantity", "average_cost", "filled", "reserved", "remaining", "done", "cash_budget", "spent"]
ORDER_KEYS = ["account_id", "quantity", "limit_price", "state", "cumulative_filled", "cancel_intent", "broker_order_id", "prepared",
              "aggressive", "dispatch_prepared", "sequence"]
ACTION_KEYS = ["type", "account_id", "symbol", "quantity", "limit_price", "side", "order_type", "time_in_force", "send_time",
               "send_timestamp", "deadline", "include_open_orders_and_executions", "trading_mode", "broker_order_id"]
COMMAND_KEYS = ["type", "account_id", "symbol", "side", "qty_shares", "limit_price", "trading_mode", "time_in_force",
                "fast_trading_resting_side", "fast_trading_total_resting_qty", "fast_trading_target_percentage",
                "fast_trading_target_qty", "fast_trading_book_levels", "fast_trading_price_limit",
                "fast_trading_total_target_position", "algo_cycle_id", "cancel_unfilled", "broker", "test_mode"]


def jsonable(value):
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, dict):
        return {str(k): jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonable(v) for v in value]
    return value


def kwargs_for(callable_obj, **candidates):
    params = inspect.signature(callable_obj).parameters
    return {k: v for k, v in candidates.items() if k in params}


class Session:
    def __init__(self, config, engine_kind="sell"):
        from trading_ui.services.smart_selling import SmartSellingEngine
        from trading_ui.services.smart_selling_feedback import SmartSellingFeedback
        from trading_ui.services.smart_selling_market import SmartSellingMarketAdapter

        if engine_kind == "buy":
            from trading_ui.services.smart_buying import SmartBuyingEngine
            self.Engine = SmartBuyingEngine
        else:
            self.Engine = SmartSellingEngine
        self.buying = engine_kind == "buy"
        self.Feedback, self.Market = SmartSellingFeedback, SmartSellingMarketAdapter
        self.dir = tempfile.mkdtemp(prefix="py-oracle-")
        self.now = 0.0
        self.config = config or {}
        self.parents = []
        self.orders = []
        self.order_index = {}
        self.submit_by_id = {}
        self.report_sequence = {}
        self.open()

    def wall(self):
        return EPOCH + self.now

    def open(self):
        self.engine = self.Engine(os.path.join(self.dir, "smart.sqlite3"), clock=lambda: self.now, wall_clock=lambda: self.wall())
        self.feedback = self.Feedback(self.engine, **kwargs_for(self.Feedback.__init__, max_cached_orders=10000,
                                                                state_path=os.path.join(self.dir, "feedback.sqlite3")))
        self.market = self.Market(
            self.engine, clock=lambda: self.now, wall_clock=lambda: self.wall(),
            max_source_age=self.config.get("max_source_age", 10), rate_window_seconds=self.config.get("rate_window", 300),
            max_observations=self.config.get("max_observations", 256), resume_cooldown_seconds=self.config.get("resume_cooldown", 10))

    def close(self):
        try:
            self.feedback.close()
        except Exception:  # noqa: BLE001
            pass
        self.engine.close()

    def register_action(self, action):
        cid = action.get("client_order_id") if isinstance(action, dict) else None
        if not isinstance(cid, str):
            return
        if cid not in self.order_index:
            self.order_index[cid] = len(self.orders)
            self.orders.append(cid)
        if action.get("type") == "submit":
            self.submit_by_id[cid] = action

    def active_order(self, step):
        """The outstanding child of a parent, resolved at run time (None when there is none)."""
        k = step.get("parent", 0)
        if k < 0 or k >= len(self.parents):
            raise RuntimeError("unknown parent index")
        order = self.engine.get_parent(self.parents[k]).get("active_order")
        if not isinstance(order, dict):
            return None
        cid = order["client_order_id"]
        if cid not in self.order_index:
            self.order_index[cid] = len(self.orders)
            self.orders.append(cid)
        return order

    def next_sequence(self, cid):
        self.report_sequence[cid] = self.report_sequence.get(cid, 0) + 1
        return self.report_sequence[cid]

    def order_id(self, ref):
        if not isinstance(ref, int) or isinstance(ref, bool) or ref < 0 or ref >= len(self.orders):
            return None
        return self.orders[ref]

    def normalise_action(self, action):
        self.register_action(action)
        out = {k: jsonable(action[k]) for k in ACTION_KEYS if k in action}
        cid = action.get("client_order_id")
        if isinstance(cid, str):
            out["order"] = self.order_index[cid]
        return out

    def normalise_parent(self, parent):
        out = {k: jsonable(parent[k]) for k in PARENT_KEYS if k in parent}
        accounts = parent.get("accounts")
        if isinstance(accounts, dict):
            out["accounts"] = {aid: {k: jsonable(a[k]) for k in ACCOUNT_KEYS if k in a} for aid, a in accounts.items()}
        if "active_order" in parent:
            order = parent["active_order"]
            if isinstance(order, dict):
                o = {k: jsonable(order[k]) for k in ORDER_KEYS if k in order}
                cid = order.get("client_order_id")
                o["order"] = self.order_index.get(cid) if isinstance(cid, str) else None
                out["active_order"] = o
            else:
                out["active_order"] = None
        return out

    def parents_snapshot(self):
        out = []
        for pid in self.parents:
            try:
                out.append(self.normalise_parent(self.engine.get_parent(pid)))
            except Exception as exc:  # noqa: BLE001
                out.append({"error": str(exc)})
        return out

    def substitute(self, value):
        if isinstance(value, str):
            for prefix in ("$order_id:", "$order_qty:", "$order_px:", "$order_part:"):
                if value.startswith(prefix):
                    rest = value[len(prefix):].split(":")
                    k = int(rest[0])
                    cid = self.order_id(k)
                    if cid is None:
                        return f"unknown-order-{k}"
                    if prefix == "$order_id:":
                        return cid
                    submit = self.submit_by_id.get(cid)
                    if submit is None:
                        return None
                    if prefix == "$order_qty:":
                        return float(submit["quantity"])
                    if prefix == "$order_px:":
                        return float(submit["limit_price"])
                    fraction = Decimal(rest[1]) if len(rest) > 1 else Decimal("0.5")
                    return str(int(Decimal(str(submit["quantity"])) * fraction))
            return value
        if isinstance(value, dict):
            return {k: self.substitute(v) for k, v in value.items()}
        if isinstance(value, list):
            return [self.substitute(v) for v in value]
        return value

    def quote_state(self, symbol):
        quotes = getattr(self.engine, "_quotes", {})
        q = quotes.get(symbol)
        if q is None:
            return None
        if isinstance(q, dict):  # the Python engine keeps quote state in plain dicts
            return {"healthy": bool(q.get("healthy", False)), "gap": bool(q.get("sequence_gap", False))}
        return {"healthy": bool(getattr(q, "healthy", False)), "gap": bool(getattr(q, "sequence_gap", False))}


def market_state_of(status):
    if isinstance(status, dict):
        return status.get("state")
    if isinstance(status, (tuple, list)):
        return status[0]
    return getattr(status, "state", status)


def source_of(step, session, key="source_time"):
    value = step.get(key, "wall")
    return session.wall() if value == "wall" else value


def run_smart_step(s, step):
    op = step.get("op", "")
    result = {}
    if op == "now":
        s.now = float(step["t"])
        result["now"] = s.now
    elif op == "start":
        parent = s.engine.start_parent(step["settings"])
        s.parents.append(parent["parent_id"])
        result["parent"] = len(s.parents) - 1
        result["created"] = s.normalise_parent(parent)
    elif op == "quote":
        kw = {"source_time": source_of(step, s)}
        for key in ("sequence", "healthy", "accessible", "reset_sequence", "preserve_consumption", "ask_size", "bid_prices",
                    "ask_prices"):
            if key in step and step[key] is not None:
                kw[key] = step[key]
        s.engine.on_quote(step.get("symbol", "XYZ"), step["bid"], step["ask"], step["bid_size"], **kw)
    elif op == "costs":
        k = step.get("parent", 0)
        if k < 0 or k >= len(s.parents):
            raise RuntimeError("unknown parent index")
        s.engine.set_account_costs(s.parents[k], dict(step["costs"]))
    elif op == "cash":
        if not s.buying:
            raise RuntimeError("cash limits apply to Smart buying only")
        for account, value in step["limits"].items():
            s.engine.cash_limits[account] = Decimal(str(value))
    elif op == "feed_health":
        s.engine.set_feed_health(step.get("symbol", "XYZ"), step.get("healthy", True), step.get("reason", ""),
                                 reset_sequence=step.get("reset_sequence", False))
    elif op == "trade":
        kw = {"source_time": source_of(step, s)}
        for key in ("sequence", "correction_of", "healthy", "interval_start"):
            if key in step and step[key] is not None:
                kw[key] = step[key]
        s.engine.on_trade(step.get("symbol", "XYZ"), step["quantity"], step["event_id"], **kw)
    elif op == "tick":
        result["actions"] = [s.normalise_action(a) for a in s.engine.tick()]
    elif op == "drain":
        result["actions"] = [s.normalise_action(a) for a in s.engine.drain_actions()]
    elif op == "report":
        cid = s.order_id(step["order"])
        if cid is None:
            raise RuntimeError("unknown order index")
        kw = {}
        for key in ("broker_order_id", "sequence", "authoritative", "final"):
            if key in step and step[key] is not None:
                kw[key] = step[key]
        s.engine.on_order_report(cid, step["status"], s.substitute(step["cumulative"]), **kw)
    elif op == "action_error":
        cid = s.order_id(step["order"])
        if cid is None:
            raise RuntimeError("unknown order index")
        s.engine.on_action_error(cid, step.get("action", "submit"), step.get("error", "error"), definitive=step.get("definitive", False))
    elif op == "prepare":
        cid = s.order_id(step["order"])
        if cid is None:
            raise RuntimeError("unknown order index")
        prepared = s.engine.prepare_dispatch(cid)
        result["prepared"] = s.normalise_action(prepared) if prepared else None
    elif op == "abort":
        cid = s.order_id(step["order"])
        if cid is None:
            raise RuntimeError("unknown order index")
        result["aborted"] = bool(s.engine.abort_dispatch(cid, step.get("reason", "abort"), stop_parent=step.get("stop_parent", False)))
    elif op == "hold":
        cid = s.order_id(step["order"])
        if cid is None:
            raise RuntimeError("unknown order index")
        result["held"] = bool(s.engine.hold_order(cid, step.get("reason", "conflict")))
    elif op == "stop":
        k = step.get("parent", 0)
        if k < 0 or k >= len(s.parents):
            raise RuntimeError("unknown parent index")
        result["stopped"] = s.normalise_parent(s.engine.request_stop(s.parents[k], reason=step.get("reason", "manual stop")))
    elif op == "forecast":
        k = step.get("parent", 0)
        if k < 0 or k >= len(s.parents):
            raise RuntimeError("unknown parent index")
        quantity = step.get("quantity")
        s.engine.set_remaining_volume_forecast(s.parents[k], None if quantity is None else Decimal(str(quantity)))
    elif op == "restart":
        s.close()
        s.open()
    elif op in ("price_book", "volume"):
        kw = {k: step[k] for k in ("topic", "partition", "offset") if k in step and step[k] is not None}
        fn = s.market.apply_price_book if op == "price_book" else s.market.apply_volume
        result["accepted"] = bool(fn(step["payload"], **kw))
        symbol = step.get("symbol", "XYZ")
        result["market_state"] = market_state_of(s.market.market_status(symbol))
        rate = s.market.volume_rate(symbol)
        result["volume_rate"] = None if rate is None else str(rate)
        q = s.quote_state(symbol)
        if q is not None:
            result["quote_healthy"] = q["healthy"]
            result["quote_gap"] = q["gap"]
    elif op == "connected":
        s.market.set_connected(step.get("connected", True))
    elif op == "volume_connected":
        s.market.set_volume_connected(step.get("connected", True))
    elif op == "snapshot":
        from trading_ui.services.smart_selling_feedback import parse_smart_selling_snapshot
        payload = s.substitute(step["payload"])
        result["applied"] = bool(s.feedback.apply_snapshot(parse_smart_selling_snapshot(payload)))
    elif op == "external":
        s.feedback.track_external_order(s.substitute(step["command"]))
    elif op == "open_orders":
        result["open"] = bool(s.feedback.has_open_orders(step.get("account_id", "A"), step.get("symbol", "XYZ")))
    elif op == "reconcile":
        cid = s.order_id(step["order"])
        if cid is None:
            raise RuntimeError("unknown order index")
        result["reconciled"] = bool(s.feedback.reconcile(cid))
    elif op in ("prepare_active", "abort_active", "report_active", "snapshot_active"):
        order = s.active_order(step)
        if order is None:
            result["skipped"] = True
        else:
            cid = order["client_order_id"]
            index = s.order_index[cid]
            result["order"] = index
            whole = (Decimal(str(order["quantity"])) * Decimal(str(step.get("fraction", "0")))).to_integral_value(rounding=ROUND_FLOOR)
            if op == "prepare_active":
                prepared = s.engine.prepare_dispatch(cid)
                result["prepared"] = s.normalise_action(prepared) if prepared else None
            elif op == "abort_active":
                result["aborted"] = bool(s.engine.abort_dispatch(cid, step.get("reason", "abort"), stop_parent=step.get("stop_parent", False)))
            elif op == "report_active":
                s.engine.on_order_report(cid, step["status"], str(whole), broker_order_id=f"broker-{index}",
                                         sequence=s.next_sequence(cid), authoritative=step.get("authoritative", True),
                                         final=step.get("final", False))
                result["cumulative"] = str(whole)
            else:
                from trading_ui.services.smart_selling_feedback import parse_smart_selling_snapshot
                from datetime import datetime, timezone
                stamp = lambda ts: datetime.fromtimestamp(ts, tz=timezone.utc).isoformat()
                payload = {"schema_version": 1, "account_id": order["account_id"], "order_id": f"broker-{index}",
                           "sequence": s.next_sequence(cid), "created_at": stamp(order["send_wall"]),
                           "updated_at": stamp(max(s.wall(), order["send_wall"])), "symbol": "XYZ",
                           "side": step.get("side", "BUY" if s.buying else "SELL"), "order_type": "LIMIT",
                           "status": step["status"], "quantity": float(order["quantity"]), "filled_quantity": float(whole),
                           "average_fill_price": float(order["limit_price"]) if whole else None,
                           "limit_price": float(order["limit_price"]), "command_id": cid}
                result["applied"] = bool(s.feedback.apply_snapshot(parse_smart_selling_snapshot(payload)))
                result["filled_quantity"] = str(whole)
    elif op == "status":
        pass
    else:
        raise RuntimeError(f"unknown op {op}")
    return result


def run_smart(scenario):
    s = Session(scenario.get("config"), scenario.get("engine", "sell"))
    trace = []
    for index, step in enumerate(scenario["steps"]):
        entry = {"step": index, "op": step.get("op", "")}
        try:
            entry["result"] = run_smart_step(s, step)
            entry["error"] = None
        except Exception as exc:  # noqa: BLE001
            entry["result"] = {}
            entry["error"] = str(exc)
        entry["parents"] = s.parents_snapshot()
        trace.append(entry)
    s.close()
    shutil.rmtree(s.dir, ignore_errors=True)
    return trace


def run_fast_case(c):
    from trading_ui.models import AccountMeta, AccountSnapshot, Position
    from trading_ui.services.fast_trading import build_fast_trading_cycle_commands
    from trading_ui.services.market_data import BookLevel, OrderBookSnapshot

    metas = {aid: AccountMeta(id=aid, num_id=m.get("num_id", 0), broker=m.get("broker", "Tiger"), trading_medium=m.get("trading_medium", "API"))
             for aid, m in c["metas"].items()}
    book = OrderBookSnapshot(symbol=c.get("symbol", "XYZ"),
                             bids=[BookLevel(price=p, quantity=q) for p, q in c["book"].get("bids", [])],
                             asks=[BookLevel(price=p, quantity=q) for p, q in c["book"].get("asks", [])])
    snapshots = None
    if c.get("snapshots") is not None:
        snapshots = {}
        for aid, snap in c["snapshots"].items():
            positions = [Position(symbol=p[0], qty=p[1], available_qty=(p[2] if len(p) > 2 else None)) for p in snap.get("positions", [])]
            snapshots[aid] = AccountSnapshot(account_id=aid, cash=snap.get("cash", 0.0),
                                             cash_by_currency=dict(snap.get("cash_by_currency", {})),
                                             available_cash_by_currency=dict(snap.get("available_cash_by_currency", {})),
                                             positions=positions, trading_enabled=snap.get("trading_enabled", True))
    r = build_fast_trading_cycle_commands(
        c.get("trading_mode", "E"), c.get("symbol", "XYZ"), book, c.get("price_limit", 0.0), c.get("account_ids", []),
        c.get("book_levels", 1), metas, c.get("target_percentage", 100), account_snapshots=snapshots,
        minimum_cycle_seconds_by_medium=c.get("minimum_cycle_seconds_by_medium"),
        last_action_monotonic_by_account=c.get("last_action"), now_monotonic=c.get("now"), cycle_id="cycle",
        total_target_position=c.get("total_target_position"))
    return {
        "total_position": r.total_position, "total_target_position": r.total_target_position,
        "target_position_reached": r.target_position_reached, "book_levels": r.book_levels,
        "target_percentage": r.target_percentage,
        "resting_side": r.resting_side, "order_side": r.order_side, "total_resting_qty": r.total_resting_qty,
        "target_quantity": r.target_quantity, "limit_price": r.limit_price, "per_account_qty": r.per_account_qty,
        "allocated_quantity": r.allocated_quantity, "cooldown_eligible_account_ids": list(r.cooldown_eligible_account_ids),
        "capacity_eligible_account_ids": list(r.capacity_eligible_account_ids), "capacity_noop": r.capacity_noop,
        "reason": r.reason, "commands": [{k: jsonable(cmd[k]) for k in COMMAND_KEYS if k in cmd} for cmd in r.commands],
    }


def run_fast(scenario):
    trace = []
    for index, c in enumerate(scenario["cases"]):
        entry = {"case": index}
        try:
            entry["result"] = run_fast_case(c)
            entry["error"] = None
        except Exception as exc:  # noqa: BLE001
            entry["result"] = None
            entry["error"] = str(exc)
        trace.append(entry)
    return trace


def main():
    scenario = json.load(sys.stdin)
    trace = run_fast(scenario) if scenario.get("kind") == "fast" else run_smart(scenario)
    json.dump(trace, sys.stdout, default=str)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
