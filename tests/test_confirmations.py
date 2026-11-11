#!/usr/bin/env python3
"""Exercise confirmation controls in Chromium, with every API request mocked.

Run: python3 tests/test_confirmations.py --browser /path/to/chromium
No application server, credentials, Kafka connection, or Python packages are needed.
"""

import argparse
import mimetypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import subprocess
import tempfile
import threading


ROOT = Path(__file__).resolve().parents[1]
HARNESS = r"""
const requests = [];
const errors = [];
window.addEventListener("error", e => errors.push(e.message));
window.addEventListener("unhandledrejection", e => errors.push(String(e.reason)));
window.EventSource = class { addEventListener() {} close() {} };
window.fetch = async (url, options = {}) => {
  const path = new URL(url, location.origin).pathname;
  let body;
  if (options.method === "POST") {
    requests.push({path, body: JSON.parse(options.body)});
    body = {ok: true, message: "Request accepted"};
  } else {
    const fixtures = {
      "/api/i18n": {confirm: "Confirm", fast_config_cancel: "Cancel"},
      "/api/bootstrap": {user: "lmh", symbols: ["TANH"], market_data_symbols: ["TANH"],
        limit_share_shortcuts: [100], through_market_choices: [1, 5],
        accounts: [], can_manage_trading: true, can_access_admin_control: true,
        can_access_market_data: true, smart_selling_available: true, smart_buying_available: true},
      "/api/accounts/view": {accounts: [], portfolio: {}},
      "/api/quotes": {rows: {}},
      "/api/algo": {active: false, fast: [], smart: {parents: []}, buying: {parents: []}},
      "/api/health": {feeds: {}},
    };
    if (!(path in fixtures)) throw new Error("Unexpected GET " + path);
    body = fixtures[path];
  }
  return {ok: true, status: 200, json: async () => body};
};
const pause = () => new Promise(resolve => setTimeout(resolve, 20));
function check(condition, message) { if (!condition) throw new Error(message); }
async function until(condition, message) {
  for (let n = 0; n < 100; n++) { if (condition()) return; await pause(); }
  throw new Error(message);
}
window.addEventListener("load", async () => {
  const result = document.createElement("pre");
  result.id = "confirmation-test-result";
  document.body.append(result);
  try {
    await until(() => document.querySelector("#stop_reason"), "Trading page did not load");
    const reason = document.querySelector("#stop_reason");
    reason.value = "Operator requested stop";
    const stop = reason.closest("form").querySelector("button[type=submit]");
    const modal = () => document.querySelector(".modal-backdrop");
    // Every dismissal must close the dialog without sending a request.
    const dismissals = [
      () => document.querySelector(".modal-actions .gray").click(),
      () => document.querySelector(".modal-close").click(),
      () => document.dispatchEvent(new KeyboardEvent("keydown", {key: "Escape", bubbles: true})),
      () => modal().click(),
    ];
    for (const dismiss of dismissals) {
      stop.click();
      check(modal(), "Stop did not ask for confirmation");
      check(requests.length === 0, "Stop submitted before confirmation");
      dismiss();
      await pause();
      check(!modal(), "Dismissal left the modal open");
      check(requests.length === 0, "Dismissal submitted a request");
      check(!stop.disabled, "Dismissal disabled Stop");
    }
    stop.click();
    document.querySelector(".modal-actions .red").click();
    await until(() => requests.length === 1, "Confirm did not send the stop request");
    check(requests[0].path === "/api/algo/stop", "Wrong stop endpoint");
    check(requests[0].body.reason === reason.value, "Stop reason was lost");
    await until(() => !stop.disabled, "Stop button stayed disabled");
    check(!modal(), "Confirmed modal stayed open");
    check(reason.closest("form").textContent.includes("Request accepted"), "Stop result was not shown");

    document.querySelector("#cancel-open-orders-form button[type=submit]").click();
    check(modal() && requests.length === 1, "Cancel-all bypassed confirmation");
    document.querySelector(".modal-actions .red").click();
    await until(() => requests.length === 2, "Cancel-all confirmation failed");
    check(requests[1].path === "/api/orders/cancel", "Wrong cancel endpoint");
    check(JSON.stringify(requests[1].body.account_ids) === '["__ALL__"]', "Cancel scope changed");

    document.querySelector('#nav a[href="/trading-status"]').click();
    document.querySelector(".inline-actions button").click();
    check(modal() && requests.length === 2, "Trading-off bypassed confirmation");
    document.querySelector(".modal-actions .red").click();
    await until(() => requests.length === 3, "Trading-off confirmation failed");
    check(requests[2].path === "/api/trading-status", "Wrong trading status endpoint");
    check(requests[2].body.disable_all && requests[2].body.trading_enabled === false, "Trading-off payload changed");
    await pause();
    check(requests.length === 3, "Duplicate requests sent");
    check(errors.length === 0, errors.join("; "));
    result.textContent = "PASS: stop, cancel-all, trading-off, and four dismissal paths";
  } catch (error) {
    result.textContent = "FAIL: " + error.message + (errors.length ? "; " + errors.join("; ") : "");
  }
});
"""


def run_browser(browser, harness, static=None, path="/control-panel", width=1280):
    static = static or ROOT / "web/static"
    html = (static / "index.html").read_text().replace(
        "<head>", "<head><script>" + harness + "</script>", 1,
    ).encode()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            request_path = self.path.split("?", 1)[0]
            asset = (static / request_path.removeprefix("/static/")).resolve()
            if request_path == path:
                data, content_type = html, "text/html"
            elif request_path.startswith("/static/") and asset.is_relative_to(static.resolve()) and asset.is_file():
                data, content_type = asset.read_bytes(), mimetypes.guess_type(asset)[0] or "application/octet-stream"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *_):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="nextgen-confirmations-") as profile:
                completed = subprocess.run(
                    [browser, "--headless", "--no-sandbox", "--disable-gpu", f"--window-size={width},900",
                     "--disable-background-networking", "--no-first-run",
                     "--user-data-dir=" + profile, "--dump-dom", "--virtual-time-budget=5000",
                     f"http://127.0.0.1:{server.server_port}{path}"],
                    capture_output=True, text=True, timeout=20,
                )
        finally:
            server.shutdown()
            thread.join()
    match = re.search(r'<pre id="confirmation-test-result">([^<]*)</pre>', completed.stdout)
    message = match[1] if match else "FAIL: browser did not complete the test\n" + completed.stderr
    print(message)
    return 0 if completed.returncode == 0 and message.startswith("PASS:") else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", required=True, help="Chromium or Chrome executable")
    return run_browser(parser.parse_args().browser, HARNESS)


if __name__ == "__main__":
    raise SystemExit(main())
