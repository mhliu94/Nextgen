#!/usr/bin/env python3
"""Browser regression checks for session reports and mobile live-table updates.

Run with --browser /path/to/chromium. All API and event-stream data is synthetic.
"""
import argparse

from test_confirmations import ROOT, run_browser


COMMON = r"""
const errors = [];
window.addEventListener("error", e => errors.push(e.message));
window.addEventListener("unhandledrejection", e => errors.push(String(e.reason)));
const pause = () => new Promise(resolve => setTimeout(resolve, 30));
function check(value, message) { if (!value) throw new Error(message); }
async function until(test, message) {
  for (let i = 0; i < 100; i++) { if (test()) return; await pause(); }
  throw new Error(message + (errors.length ? "; " + errors.join("; ") : ""));
}
function run(test) {
  window.addEventListener("load", async () => {
    const result = document.createElement("pre");
    result.id = "confirmation-test-result";
    document.body.append(result);
    try {
      await test();
      check(!errors.length, errors.join("; "));
      result.textContent = "PASS: " + document.documentElement.dataset.testName;
    } catch (error) { result.textContent = "FAIL: " + error.message; }
  });
}
"""

ORDERS = COMMON + r"""
const account = {account_id: "A", account_num_id: 1, broker: "Tiger", shares_bought: 10, shares_sold: 8,
  cash_by_currency: {USD: {shares_bought: 10, shares_sold: 8, buy_cost: 175, sell_proceeds: 300,
    cash_change: 125, complete: true, average_buy_price: 17.5, average_sell_price: 37.5},
    HKD: {shares_bought: 0, shares_sold: 1, buy_cost: 0, sell_proceeds: 200.126,
      cash_change: 200.126, complete: true, average_buy_price: null, average_sell_price: 200.126}}};
const sessions = Array.from({length: 25}, (_, i) => ({id: "session-" + i, display_id: (i === 24 ? "BIYA" : "TANH") + "-G-" + (i + 1),
  symbol: i === 24 ? "BIYA" : "TANH", account_ids: ["A"],
  parameters: {trading_mode: "G", minimum_price: 38, account_ids: ["A"]},
  status: i === 0 ? "running" : "stopped", running: i === 0,
  started_at: new Date(Date.UTC(2026, 10, 11, 15, 0) - i * 60000).toISOString(),
  ended_at: i === 0 ? null : "2026-11-11T15:30:00Z", updated_at: "2026-11-11T15:30:00Z",
  reason: i === 0 ? "waiting for market volume" : "manual stop", accounts: [account], totals: account}));
sessions[23].id = '<img src=x onerror=alert(1)>';
const data = {username: "tester", date: "2026-11-11", timezone: "America/New_York", refresh_interval_seconds: 60,
  accounts: [], statuses: [], orders: [], feed: {state: "live", detail: "Kafka connected", invalid_messages: 0},
  reporting: {sessions, tags: [], active_tag_id: null}};
window.fetch = async (url, options = {}) => {
  check(!options.method || options.method === "GET", "Unexpected mutation");
  check(url === "/api/orders", "Unexpected API " + url);
  return {ok: true, status: 200, json: async () => structuredClone(data)};
};
run(async () => {
  document.documentElement.dataset.testName = "session pagination, selection, averages, translation, and mobile table layout";
  await until(() => !document.getElementById("dashboard").hidden, "Orders did not load");
  const get = id => document.getElementById(id);
  const rows = () => [...get("algo-rows").children];
  const search = value => { get("algo-search").value = value; get("algo-search").dispatchEvent(new Event("input")); };
  get("tab-algo").click();
  check(rows().length === 10, "Expected ten sessions on first page");
  check(rows()[0].textContent.includes("TANH-G-1"), "Public session ID missing in order UI");
  check(get("algo-previous").disabled && !get("algo-next").disabled, "Incorrect first-page controls");
  check(get("algo-page").textContent.includes("1") && get("algo-page").textContent.includes("3"), "Page count incorrect");
  get("algo-next").click();
  check(rows().length === 10 && rows()[0].textContent.includes("TANH-G-11"), "Second page incorrect");
  rows()[4].querySelector("button").click();
  await refresh();
  check(get("algo-summary").textContent.includes("TANH-G-15"), "Selection lost after polling");
  get("algo-next").click();
  check(rows().length === 5 && get("algo-next").disabled, "Last page incorrect");
  check(!get("algo-rows").querySelector("img"), "Session ID interpreted as HTML");
  search("BIYA");
  check(rows().length === 1 && get("algo-summary").textContent.includes("BIYA-G-25"), "Session search failed");
  search("nothing-matches");
  check(rows().length === 0 && !get("algo-empty").hidden && get("algo-report").hidden, "Empty search state failed");
  search("");
  get("algo-status").value = "running";
  get("algo-status").dispatchEvent(new Event("input"));
  check(rows().length === 1, "Status filter failed");
  get("algo-status").value = "";
  get("algo-status").dispatchEvent(new Event("input"));
  for (const term of get("algo-parameters").querySelectorAll("dt"))
    check(Number(getComputedStyle(term).fontWeight) >= 700, "Starting parameter label is not bold");
  const cells = () => get("algo-accounts").querySelector("tbody tr").cells;
  check(cells().length === 8, "Average-price columns missing");
  check(cells()[2].textContent.includes("USD 17.50"), "Buy average not formatted to two decimals");
  check(cells()[4].textContent.includes("HKD 200.13") && cells()[4].textContent.includes("USD 37.50"), "Sell averages incorrect");
  check(get("algo-accounts").querySelector("tfoot").textContent.includes("USD 17.50"), "Total average missing");
  account.cash_by_currency.USD.average_buy_price = null;
  account.cash_by_currency.USD.buy_cost = null;
  account.cash_by_currency.USD.complete = false;
  await refresh();
  check(cells()[2].textContent.includes("Incomplete price data"), "Missing buy price not marked incomplete");
  check(!cells()[4].textContent.includes("Incomplete"), "Missing buy price contaminated sell average");
  document.querySelector('[data-language="zh"]').click();
  check(get("algo-accounts").textContent.includes("买入均价"), "Chinese average label missing");
  check(get("algo-rows").textContent.includes("智能卖出"), "Chinese mode label missing");
  if (innerWidth < 500) {
    check(document.documentElement.scrollWidth <= innerWidth + 1, "Mobile page overflows horizontally");
    const scroll = get("algo-accounts");
    check(scroll.scrollWidth > scroll.clientWidth, "Mobile execution table is not horizontally scrollable");
    scroll.scrollLeft = 120;
    await refresh();
    check(scroll.scrollLeft === 120, "Report scroll position lost during polling");
    check(getComputedStyle(get("algo-rows").firstChild).display === "table-row", "Sessions became cards on mobile");
  }
  data.reporting.sessions = [];
  await refresh();
  check(rows().length === 0 && get("algo-report").hidden && !get("algo-empty").hidden, "Empty history state failed");
});
"""

ADMIN = COMMON + r"""
// Chromium's dump-DOM virtual clock does not reliably advance compositor frames.
window.requestAnimationFrame = fn => setTimeout(() => fn(performance.now()), 16);
const listeners = {};
window.EventSource = class {
  addEventListener(name, fn) { listeners[name] = fn; }
  close() {}
};
const parent = {parent_id: "parent-1", display_id: "TANH-G-11", account_id: "A", account_ids: ["A", "B", "C"], symbol: "TANH",
  active: true, state: "RUNNING", reason: "earning volume credit", market_state: "TRADING", market_reason: "",
  quantity: "10000", filled: "123", remaining: "9877", reserved: "0", free_credit: "80", mode: "urgent",
  participation: "0.2", child_cap: "100", start: new Date().toISOString(), accounts: {}, minimum_price: "38"};
const accounts = {accounts: [], metas: [], stock_quotes: {}, generated_at: new Date().toISOString(),
  portfolio: {cash: {USD: 1000}, included_accounts: 0, total_accounts: 0, total_market_value: 1234567,
    positions: [{symbol: "TANH", qty: 123456, avg_price: 38.1234, last: 39.1234, market_value: 4830000, account_count: 12}]}};
const algo = {active: true, fast: [], smart: {parents: [parent]}, buying: {parents: []}};
window.fetch = async (url, options = {}) => {
  check(!options.method || options.method === "GET", "Unexpected mutation");
  const fixtures = {
    "/api/i18n": {algo_waiting_volume: "Waiting for market volume", algo_waiting_reconciliation: "Waiting for broker order confirmation"},
    "/api/bootstrap": {user: "lmh", symbols: ["TANH"], market_data_symbols: ["TANH"], accounts: [],
      limit_share_shortcuts: [100], through_market_choices: [1], smart_selling_available: true, smart_buying_available: true},
    "/api/accounts/view": accounts, "/api/quotes": {rows: {}}, "/api/algo": algo, "/api/health": {feeds: {}},
  };
  const path = new URL(url, location.origin).pathname;
  check(path in fixtures, "Unexpected API " + path);
  return {ok: true, status: 200, json: async () => structuredClone(fixtures[path])};
};
run(async () => {
  document.documentElement.dataset.testName = "mobile scroll and expanded details survive live updates; current algo reason stays visible";
  await until(() => document.querySelector("#portfolio-positions"), "Portfolio did not load");
  let scroll = document.querySelector("#portfolio-positions");
  check(scroll.scrollWidth > scroll.clientWidth, "Portfolio has no mobile scroll area");
  scroll.scrollLeft = 100;
  const portfolioScroll = scroll.scrollLeft;
  accounts.portfolio.positions[0].qty++;
  listeners.accounts({data: JSON.stringify(accounts)});
  await until(() => scroll.textContent.includes("123,457"), "Portfolio update did not render");
  check(document.querySelector("#portfolio-positions") === scroll, "Portfolio scroller replaced");
  check(scroll.scrollLeft === portfolioScroll && portfolioScroll > 0, "Portfolio scroll reset");
  scroll.scrollLeft = 0;
  check(scroll.scrollLeft === 0, "Cannot scroll back to first column");
  document.querySelector('#nav a[href="/control-panel"]').click();
  await until(() => document.querySelector("#smart-status-table"), "Algo table did not load");
  check(!document.body.textContent.includes("20.00%"), "Proprietary participation rate was rendered");
  check(!document.body.textContent.includes("smart_participation"), "Participation label was rendered");
  check(document.body.textContent.includes("TANH-G-11"), "Public session ID missing in admin UI");
  for (const [mode, category] of [["e", "buying"], ["f", "selling"], ["g", "selling"], ["h", "buying"]])
    check(document.querySelector("#algo-mode-" + mode).nextSibling.querySelector("small").dataset.algoCategory === category,
      "Incorrect algorithm category for mode " + mode);
  scroll = document.querySelector("#smart-status .table-scroll");
  check(scroll.scrollWidth > scroll.clientWidth, "Algo table has no mobile scroll area");
  const details = scroll.querySelector("details");
  details.open = true;
  scroll.scrollLeft = 100;
  const sessionScroll = scroll.scrollLeft;
  const summary = document.querySelector(".smart-session-summaries");
  check(summary.textContent.includes("Waiting for market volume"), "Waiting reason not visible");
  check(!summary.closest(".table-scroll"), "Status hidden inside the wide table");
  parent.reason = "order reconciliation required";
  parent.state = "HOLD";
  parent.filled = "124";
  listeners.smart({data: JSON.stringify({parents: [parent]})});
  await until(() => summary.textContent.includes("Waiting for broker order confirmation"), "Live reason did not update");
  check(document.querySelector("#smart-status .table-scroll") === scroll, "Algo scroller replaced");
  check(scroll.scrollLeft === sessionScroll && sessionScroll > 0, "Algo scroll reset");
  check(scroll.querySelector("details").open, "Expanded session details collapsed");
  check(document.documentElement.scrollWidth <= innerWidth + 1, "Mobile admin page overflows");
  scroll.scrollLeft = 0;
  check(scroll.scrollLeft === 0, "Cannot scroll algo table back to the left");
});
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", required=True)
    browser = parser.parse_args().browser
    results = [run_browser(browser, ORDERS, ROOT / "web/static/orders", "/", width)
               for width in (1440, 390)]
    results.append(run_browser(browser, ADMIN, path="/account-details", width=390))
    return int(any(results))


if __name__ == "__main__":
    raise SystemExit(main())
