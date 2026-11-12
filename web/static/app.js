/* KTrader next-generation control center — single-page application (no build step). */
(function () {
  "use strict";

  // ------------------------------------------------------------------------------------------
  // Utilities
  // ------------------------------------------------------------------------------------------
  const $ = (sel, root) => (root || document).querySelector(sel);
  const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));

  function h(tag, attrs, ...children) {
    const el = document.createElement(tag);
    if (attrs) {
      for (const [k, v] of Object.entries(attrs)) {
        if (v === null || v === undefined || v === false) continue;
        if (k === "class") el.className = v;
        else if (k === "html") el.innerHTML = v;
        else if (k === "text") el.textContent = v;
        else if (k.startsWith("on") && typeof v === "function") el.addEventListener(k.slice(2), v);
        else if (k === "dataset") Object.assign(el.dataset, v);
        else if (v === true) el.setAttribute(k, "");
        else el.setAttribute(k, String(v));
      }
    }
    for (const child of children.flat(Infinity)) {
      if (child === null || child === undefined || child === false) continue;
      el.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return el;
  }

  function updateScrollableTable(scroll, table) {
    const left = scroll.scrollLeft, top = scroll.scrollTop;
    const expanded = new Set($$("details[data-parent-id][open]", scroll).map((item) => item.dataset.parentId));
    for (const detail of $$("details[data-parent-id]", table)) detail.open = expanded.has(detail.dataset.parentId);
    scroll.replaceChildren(table);
    scroll.scrollLeft = left;
    scroll.scrollTop = top;
  }

  function algoReason(reason) {
    const keys = {
      "waiting for eligible tape volume": "algo_waiting_volume",
      "earning volume credit": "algo_waiting_volume",
      "earned credit below broker minimum": "algo_waiting_volume_minimum",
      "waiting for volume credit and at least 100 shares of liquidity": "algo_waiting_volume_liquidity",
      "waiting for live order and volume feeds": "algo_waiting_feeds",
      "waiting for fresh trading-volume baseline": "algo_waiting_baseline",
      "order reconciliation required": "algo_waiting_reconciliation",
      "waiting for child terminal status": "algo_waiting_order",
      "minimum five-second send spacing": "algo_waiting_spacing",
      "account snapshot unavailable or trading disabled": "algo_waiting_account",
    };
    return keys[reason] ? t(keys[reason]) : (reason || "—");
  }

  const fmt = {
    money(v, digits = 2) {
      const n = Number(v);
      if (v === null || v === undefined || v === "" || !Number.isFinite(n)) return "—";
      return "$" + n.toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
    },
    num(v, digits = 2) {
      const n = Number(v);
      if (v === null || v === undefined || v === "" || !Number.isFinite(n)) return "—";
      return n.toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
    },
    price(v) { return fmt.num(v, 4); },
    qty(v) {
      const n = Number(v);
      if (v === null || v === undefined || v === "" || !Number.isFinite(n)) return "—";
      return n.toLocaleString(undefined, { maximumFractionDigits: 8 });
    },
    int(v) {
      const n = Number(v);
      if (v === null || v === undefined || !Number.isFinite(n)) return "—";
      return Math.trunc(n).toLocaleString();
    },
    pct(v) {
      const n = Number(v);
      if (v === null || v === undefined || !Number.isFinite(n)) return "—";
      return (n >= 0 ? "+" : "") + n.toFixed(2) + "%";
    },
    signed(v, digits = 4) {
      const n = Number(v);
      if (v === null || v === undefined || !Number.isFinite(n)) return "—";
      return (n >= 0 ? "+" : "") + n.toLocaleString(undefined, { minimumFractionDigits: digits, maximumFractionDigits: digits });
    },
    time(iso) {
      if (!iso) return "—";
      const d = new Date(iso);
      if (Number.isNaN(d.getTime())) return String(iso);
      return d.toLocaleString(undefined, { hour12: false });
    },
    clock(iso) {
      if (!iso) return "—";
      const d = new Date(iso);
      if (Number.isNaN(d.getTime())) return String(iso);
      return d.toLocaleTimeString(undefined, { hour12: false });
    },
    epoch(sec) {
      if (!sec) return "—";
      return new Date(sec * 1000).toLocaleString(undefined, { hour12: false });
    },
    age(iso) {
      if (!iso) return t("never");
      const ms = Date.now() - new Date(iso).getTime();
      if (!Number.isFinite(ms)) return "—";
      const s = Math.max(0, Math.round(ms / 1000));
      if (s < 90) return t("seconds_ago", { seconds: s });
      return t("minutes_ago", { minutes: Math.round(s / 60) });
    },
    decimal(v) {
      if (v === null || v === undefined || v === "") return "—";
      const s = String(v);
      if (!/^-?\d+(\.\d+)?$/.test(s)) return s;
      const [i, f = ""] = s.split(".");
      const trimmed = f.replace(/0+$/, "");
      const whole = Number(i).toLocaleString();
      return trimmed ? whole + "." + trimmed : whole;
    },
  };

  // Live quote row for a symbol: the streamed rows cover the market-data symbols; anything else
  // (custom symbols typed in order dialogs) is looked up through /api/quotes and cached briefly.
  function quoteRow(symbol, onUpdate) {
    symbol = String(symbol || "").trim().toUpperCase();
    if (!symbol) return null;
    if (state.rows[symbol]) return state.rows[symbol];
    const cached = state.quoteCache.get(symbol);
    const now = Date.now();
    if (!cached || (!cached.pending && now - cached.at > 5000)) {
      const entry = { row: cached ? cached.row : null, at: now, pending: true };
      state.quoteCache.set(symbol, entry);
      api("/api/quotes?symbol=" + encodeURIComponent(symbol))
        .then((payload) => { entry.row = payload && payload.rows ? payload.rows[symbol] || null : null; })
        .catch(() => { /* keep the previous value */ })
        .finally(() => { entry.pending = false; entry.at = Date.now(); if (onUpdate) onUpdate(); });
    }
    return cached ? cached.row : null;
  }
  function lastPriceText(symbol, onUpdate) {
    const row = quoteRow(symbol, onUpdate);
    return row && !row.error && row.last !== null && row.last !== undefined ? fmt.price(row.last) : "—";
  }

  function holdingSymbol(value) {
    const symbol = String(value || "").trim().toUpperCase();
    return symbol.startsWith("US.") ? symbol.slice(3) : symbol;
  }
  function usdCash(account) {
    if (!account) return null;
    const balances = account.cash_by_currency || {};
    const key = Object.keys(balances).find((k) => String(k).trim().toUpperCase() === "USD");
    return key === undefined ? account.cash : balances[key];
  }
  function ownedShares(account, symbol) {
    symbol = holdingSymbol(symbol);
    if (!account || !symbol || !Array.isArray(account.positions)) return null;
    let total = 0;
    for (const p of account.positions) {
      if (holdingSymbol(p.symbol) !== symbol) continue;
      const q = p.qty;
      if (q === null || q === undefined || q === "" || !Number.isFinite(Number(q))) return null;
      total += Number(q);
    }
    return Number.isFinite(total) ? total : null;
  }
  function formatAccountBalance(account, selling, symbol) {
    const value = account ? (selling ? ownedShares(account, symbol) : usdCash(account)) : null;
    if (value === null || value === undefined || value === "" || !Number.isFinite(Number(value))) return "—";
    return selling ? Number(value).toLocaleString(undefined, { maximumFractionDigits: 8 }) : fmt.money(value);
  }

  const storage = {
    get(k) { try { return window.localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { window.localStorage.setItem(k, v); } catch (e) { /* ignore */ } },
  };
  function getCookie(name) {
    const m = document.cookie.match(new RegExp("(?:^|; )" + name.replace(/[.$?*|{}()[\]\\/+^]/g, "\\$&") + "=([^;]*)"));
    return m ? decodeURIComponent(m[1]) : null;
  }

  // ------------------------------------------------------------------------------------------
  // State
  // ------------------------------------------------------------------------------------------
  const state = {
    lang: "en",
    T: {},
    boot: null,       // bootstrap payload (user, accounts, symbols, …)
    accounts: null,   // accounts view payload (accounts, portfolio, metas, stock_quotes)
    rows: {},         // market data rows by symbol
    book: null,       // current insights book
    smart: null,
    algo: null,
    feeds: null,
    commands: null,
    route: "account-details",
    stream: null,
    bookStream: null,
    bookKey: "",
    midHistory: new Map(),  // symbol -> array of mids (sparklines)
    prevRows: {},
    renderQueued: new Set(),
    connected: true,
    quoteCache: new Map(),  // symbol -> { row, at } from /api/quotes?symbol= (symbols outside the live set)
    activePage: null,
  };

  function t(key, params) {
    let text = state.T[key] || key;
    if (params) for (const [k, v] of Object.entries(params)) text = text.split("{" + k + "}").join(String(v));
    return text;
  }

  // ------------------------------------------------------------------------------------------
  // Networking
  // ------------------------------------------------------------------------------------------
  class AuthError extends Error {}

  async function api(url, options = {}) {
    const init = Object.assign({ credentials: "same-origin", cache: "no-store" }, options);
    if (init.json !== undefined) {
      init.method = init.method || "POST";
      init.headers = Object.assign({ "Content-Type": "application/json" }, init.headers || {});
      init.body = JSON.stringify(init.json);
      delete init.json;
    }
    if (init.form !== undefined) {
      init.method = init.method || "POST";
      const params = new URLSearchParams();
      for (const [k, v] of Object.entries(init.form)) {
        if (Array.isArray(v)) v.forEach((x) => params.append(k, x));
        else if (v !== undefined && v !== null) params.append(k, v);
      }
      init.headers = Object.assign({ "Content-Type": "application/x-www-form-urlencoded" }, init.headers || {});
      init.body = params.toString();
      delete init.form;
    }
    init.headers = Object.assign({ Accept: "application/json" }, init.headers || {});
    const response = await fetch(url, init);
    if (response.status === 401) throw new AuthError(t("session_expired"));
    let payload = null;
    try { payload = await response.json(); } catch (e) { payload = null; }
    if (!response.ok || (payload && payload.ok === false)) {
      const message = (payload && (payload.error || payload.detail)) || t("error_generic");
      const error = new Error(message);
      error.status = response.status;
      error.payload = payload;
      throw error;
    }
    return payload;
  }

  function toast(kind, message, ttl = 6000) {
    const root = $("#toasts");
    const el = h("div", { class: "toast " + kind, role: "status" }, h("span", { text: message }),
      h("button", { class: "toast-close", type: "button", "aria-label": t("modal_close"), onclick: () => el.remove() }, "×"));
    root.append(el);
    if (ttl) setTimeout(() => el.remove(), ttl);
  }

  // ------------------------------------------------------------------------------------------
  // Modals
  // ------------------------------------------------------------------------------------------
  let activeModal = null;
  function openModal(title, body, { subtitle, actions, onClose, wide } = {}) {
    closeModal();
    const lastFocused = document.activeElement;
    const panel = h("div", { class: "modal-panel", role: "dialog", "aria-modal": "true", style: wide ? "width:min(1100px,100%)" : "" });
    const closeBtn = h("button", { class: "modal-close", type: "button", "aria-label": t("modal_close"), onclick: () => closeModal() }, "×");
    panel.append(h("div", { class: "modal-head" }, h("div", null, h("h2", { class: "modal-title", text: title }), subtitle ? h("div", { class: "help" }, subtitle) : null), closeBtn));
    panel.append(body);
    if (actions) panel.append(h("div", { class: "modal-actions" }, actions));
    const backdrop = h("div", { class: "modal-backdrop", onclick: (e) => { if (e.target === backdrop) closeModal(); } }, panel);
    $("#modal-root").append(backdrop);
    document.body.classList.add("modal-open");
    activeModal = { backdrop, onClose, lastFocused };
    const first = panel.querySelector("input:not([type=hidden]):not([disabled]), select, button.btn");
    (first || closeBtn).focus();
    return panel;
  }
  function closeModal() {
    if (!activeModal) return;
    const { backdrop, onClose, lastFocused } = activeModal;
    activeModal = null;
    backdrop.remove();
    document.body.classList.remove("modal-open");
    if (onClose) onClose();
    if (lastFocused && lastFocused.isConnected && typeof lastFocused.focus === "function") lastFocused.focus();
  }
  function confirmDialog(message) {
    return new Promise((resolve) => {
      const body = h("p", { text: message });
      openModal(t("confirm_title"), body, {
        actions: [
          h("button", { class: "btn gray", type: "button", onclick: () => { closeModal(); resolve(false); } }, t("fast_config_cancel")),
          // Settle confirmation before closeModal invokes the dismissal callback.
          h("button", { class: "btn red", type: "button", onclick: () => { resolve(true); closeModal(); } }, t("confirm")),
        ],
        onClose: () => resolve(false),
      });
    });
  }
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && activeModal) closeModal();
    if (e.key === "/" && !activeModal && !/input|select|textarea/i.test(document.activeElement.tagName)) {
      const filter = $("#account-filter");
      if (filter) { e.preventDefault(); filter.focus(); }
    }
  });

  // ------------------------------------------------------------------------------------------
  // Router & chrome
  // ------------------------------------------------------------------------------------------
  // `gate` names the bootstrap entitlement a page requires (the server enforces the same rules).
  const PAGES = [
    { id: "account-details", path: "/account-details", label: "nav_account" },
    { id: "working-orders", path: "/working-orders", label: "wo_title" },
    { id: "control-panel", path: "/control-panel", label: "nav_control" },
    { id: "market-data", path: "/market-data", label: "nav_market", gate: "can_access_market_data", forbidden: "md_forbidden" },
    { id: "market-insights", path: "/market-insights", label: "nav_insights" },
    { id: "admin-control", path: "/admin-control", label: "nav_admin", gate: "can_access_admin_control", forbidden: "admin_control_forbidden" },
    { id: "trading-status", path: "/trading-status", label: "nav_status", gate: "can_manage_trading", forbidden: "trading_status_forbidden" },
  ];
  const pageAllowed = (page) => !page.gate || !!(state.boot && state.boot[page.gate]);

  function routeFromPath(pathname) {
    const found = PAGES.find((p) => p.path === pathname);
    if (found) return found.id;
    if (pathname === "/" || pathname === "" || pathname === "/login") return "account-details";
    return null;
  }

  function navigate(pathOrId, replace = false) {
    const page = PAGES.find((p) => p.id === pathOrId || p.path === pathOrId);
    const path = page ? page.path : "/account-details";
    if (replace) history.replaceState({}, "", path); else history.pushState({}, "", path);
    setRoute();
  }
  window.addEventListener("popstate", setRoute);

  function setRoute() {
    const id = routeFromPath(location.pathname);
    if (id === null) { state.route = "not-found"; } else state.route = id;
    const page = PAGES.find((p) => p.id === state.route);
    if (page && !pageAllowed(page)) {
      toast("err", t(page.forbidden));
      navigate("control-panel", true);
      return;
    }
    renderNav();
    renderPage();
  }

  function renderNav() {
    const nav = $("#nav");
    nav.replaceChildren();
    for (const page of PAGES) {
      if (!pageAllowed(page)) continue;
      nav.append(h("a", { class: "tab" + (state.route === page.id ? " active" : ""), href: page.path, onclick: (e) => { e.preventDefault(); navigate(page.id); } }, t(page.label)));
    }
    document.documentElement.lang = state.lang;
    document.title = (state.T.title || "KTrader") + " · KTrader";
    $("#health-btn").title = t("health_title");
    $("#theme-btn").title = t("theme_toggle");
    $("#lang-btn").textContent = state.lang === "zh" ? "EN" : "中文";
    $("#lang-btn").title = state.lang === "zh" ? "Switch to English" : "切换到中文";
    $("#logout-link").textContent = t("logout");
    $("#user-chip").replaceChildren(t("signed_in_as") + ": ", h("b", { text: state.boot ? state.boot.user : "" }));
    $$("[data-i18n]").forEach((el) => { el.textContent = t(el.dataset.i18n); });
    renderFeedPills();
  }

  const ICONS = {
    sun: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>',
    moon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg>',
    pulse: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 12h4l3-8 4 16 3-8h4"/></svg>',
  };

  function applyTheme(theme) {
    document.documentElement.dataset.theme = theme;
    storage.set("ktrader.theme", theme);
    $("#theme-btn").innerHTML = theme === "dark" ? ICONS.sun : ICONS.moon;
  }

  function feedClass(feed) {
    if (!feed || !feed.state) return "";
    if (feed.state === "live") {
      const age = feed.last_message_age_seconds;
      if (age !== null && age !== undefined && age > 300) return "stale";
      return "live";
    }
    if (feed.state === "disconnected" || feed.state === "stopped") return "down";
    return "";
  }
  function renderFeedPills() {
    const root = $("#feed-pills");
    root.replaceChildren();
    const feeds = state.feeds || {};
    const items = [["accounts", "feed_accounts"], ["market", "feed_market"], ["orders", "feed_orders"], ["volume", "feed_volume"]];
    for (const [key, label] of items) {
      // Smart buying has its own order/volume consumers; show them when Smart selling is disabled.
      const feed = feeds[key] === null && feeds["buying_" + key] ? feeds["buying_" + key] : feeds[key];
      if (feed === null) continue;  // feature disabled
      const cls = feedClass(feed);
      const stateText = feed && feed.state ? t("feed_" + (cls === "stale" ? "stale" : feed.state)) : t("feed_unavailable");
      const age = feed && feed.last_message_at ? fmt.age(feed.last_message_at) : "";
      root.append(h("span", { class: "feed-pill " + cls, title: t(label) + ": " + stateText + (age ? " · " + t("last_update") + " " + age : "") },
        h("span", { class: "dot" }), h("span", { class: "lbl", text: t(label) }), h("span", { class: "state", text: stateText })));
    }
  }

  // ------------------------------------------------------------------------------------------
  // Streaming
  // ------------------------------------------------------------------------------------------
  function openStream() {
    if (state.stream) state.stream.close();
    const es = new EventSource("/api/stream?topics=accounts,quotes,smart,algo,feeds,commands");
    state.stream = es;
    es.onopen = () => setConnected(true);
    es.onerror = () => { setConnected(false); checkSession(); };
    es.addEventListener("accounts", (e) => { state.accounts = JSON.parse(e.data); queueRender("accounts"); });
    es.addEventListener("quotes", (e) => {
      const payload = JSON.parse(e.data);
      state.prevRows = state.rows;
      state.rows = payload.rows || {};
      for (const [symbol, row] of Object.entries(state.rows)) {
        if (row && typeof row.last === "number") {
          const hist = state.midHistory.get(symbol) || [];
          if (!hist.length || hist[hist.length - 1] !== row.last) { hist.push(row.last); if (hist.length > 120) hist.shift(); }
          state.midHistory.set(symbol, hist);
        }
      }
      queueRender("quotes");
    });
    es.addEventListener("smart", (e) => { state.smart = JSON.parse(e.data); queueRender("smart"); });
    es.addEventListener("algo", (e) => { state.algo = JSON.parse(e.data); queueRender("algo"); });
    es.addEventListener("feeds", (e) => { state.feeds = JSON.parse(e.data); renderFeedPills(); queueRender("feeds"); });
    es.addEventListener("commands", (e) => { state.commands = JSON.parse(e.data); queueRender("commands"); });
  }

  function openBookStream(symbol, depth) {
    const key = symbol + "|" + depth;
    if (state.bookKey === key && state.bookStream) return;
    if (state.bookStream) state.bookStream.close();
    state.bookKey = key;
    state.book = null;
    if (!symbol) return;
    const es = new EventSource("/api/stream?topics=book&book=" + encodeURIComponent(symbol) + "&depth=" + encodeURIComponent(depth));
    state.bookStream = es;
    es.addEventListener("book", (e) => { state.book = JSON.parse(e.data); queueRender("book"); });
    es.onerror = () => checkSession();
  }
  function closeBookStream() {
    if (state.bookStream) state.bookStream.close();
    state.bookStream = null;
    state.bookKey = "";
  }

  let sessionCheckAt = 0;
  async function checkSession() {
    if (Date.now() - sessionCheckAt < 5000) return;
    sessionCheckAt = Date.now();
    try {
      await api("/api/session");
    } catch (e) {
      if (e instanceof AuthError) showLogin(t("session_expired"));
    }
  }

  function setConnected(ok) {
    if (state.connected === ok) return;
    state.connected = ok;
    const banner = $("#conn-banner");
    if (ok) {
      banner.textContent = t("connection_restored");
      banner.className = "banner ok";
      banner.hidden = false;
      setTimeout(() => { if (state.connected) banner.hidden = true; }, 2500);
      refreshAll();
    } else {
      banner.textContent = t("connection_lost");
      banner.className = "banner";
      banner.hidden = false;
    }
  }

  function queueRender(topic) {
    state.renderQueued.add(topic);
    if (state.renderQueued.size === 1) requestAnimationFrame(flushRender);
  }
  function flushRender() {
    const topics = state.renderQueued;
    state.renderQueued = new Set();
    const page = pages[state.route];
    if (page && page.update) page.update(topics);
  }

  async function refreshAll() {
    try {
      const [accounts, market, algo, feeds] = await Promise.all([
        api("/api/accounts/view"), api("/api/quotes"), api("/api/algo"), api("/api/health"),
      ]);
      state.accounts = accounts;
      state.rows = market.rows || {};
      state.algo = algo;
      state.smart = algo.smart;
      state.feeds = feeds.feeds;
      renderFeedPills();
      renderPage();
    } catch (e) {
      if (e instanceof AuthError) showLogin(e.message);
    }
  }

  // ------------------------------------------------------------------------------------------
  // Pages
  // ------------------------------------------------------------------------------------------
  const pages = {};

  function renderPage() {
    const content = $("#content");
    closeModal();
    if (state.route !== "market-insights") closeBookStream();
    if (state.activePage && state.activePage.destroy) state.activePage.destroy();
    content.replaceChildren();
    const page = pages[state.route];
    state.activePage = page || null;
    if (!page) {
      content.append(h("div", { class: "card" }, h("h2", { class: "card-title", text: t("page_not_found") })));
      return;
    }
    page.render(content);
  }

  // ---- Account details -------------------------------------------------------------------
  const SORT_NUMERIC = "numeric_id", SORT_CASH = "usd_cash", SORT_SECURITY = "security";

  pages["account-details"] = {
    render(root) {
      const prefSort = storage.get("account_details_sort") || getCookie("account_details_sort") || SORT_NUMERIC;
      const prefSecurity = storage.get("account_details_security") || getCookie("account_details_security") || "";
      const symbols = state.boot.symbols || [];
      this.sort = [SORT_NUMERIC, SORT_CASH, SORT_SECURITY].includes(prefSort) ? prefSort : SORT_NUMERIC;
      this.security = symbols.includes(prefSecurity) ? prefSecurity : (symbols[0] || "");
      if (this.sort === SORT_SECURITY && !this.security) this.sort = SORT_NUMERIC;
      this.filter = "";
      this.showEmpty = storage.get("ktrader.show_empty") !== "0";

      const sortSel = h("select", { id: "account-sort", onchange: () => { this.sort = sortSel.value; storage.set("account_details_sort", this.sort); document.cookie = "account_details_sort=" + this.sort + "; path=/; max-age=15552000; samesite=lax"; secField.hidden = this.sort !== SORT_SECURITY; this.update(new Set(["accounts"])); } },
        h("option", { value: SORT_NUMERIC, text: t("sort_numeric_id") }),
        h("option", { value: SORT_CASH, text: t("sort_usd_cash") }),
        h("option", { value: SORT_SECURITY, text: t("sort_security_holdings") }));
      sortSel.value = this.sort;
      const secSel = h("select", { id: "account-sort-security", onchange: () => { this.security = secSel.value; storage.set("account_details_security", this.security); document.cookie = "account_details_security=" + encodeURIComponent(this.security) + "; path=/; max-age=15552000; samesite=lax"; this.update(new Set(["accounts"])); } },
        symbols.map((s) => h("option", { value: s, text: s })));
      secSel.value = this.security;
      const secField = h("div", { class: "field", hidden: this.sort !== SORT_SECURITY }, h("label", { for: "account-sort-security", text: t("sort_security") }), secSel);
      const filter = h("input", { id: "account-filter", type: "text", placeholder: t("search_accounts"), oninput: () => { this.filter = filter.value.trim().toLowerCase(); this.update(new Set(["accounts"])); } });
      const showEmpty = h("input", { type: "checkbox", checked: this.showEmpty, onchange: () => { this.showEmpty = showEmpty.checked; storage.set("ktrader.show_empty", this.showEmpty ? "1" : "0"); this.update(new Set(["accounts"])); } });

      root.append(
        h("div", { class: "page-head" },
          h("h1", { class: "page-title", text: t("page_account_details") }),
          h("div", { class: "toolbar" },
            h("div", { class: "field" }, h("label", { for: "account-sort", text: t("sort_accounts") }), sortSel),
            secField,
            h("div", { class: "field" }, h("label", { for: "account-filter", text: t("search_accounts") }), filter),
            h("label", { class: "check-row", style: "align-self:end;padding-bottom:8px" }, showEmpty, h("span", { text: t("show_hidden_cards") })))),
        h("div", { class: "kpis", id: "account-kpis" }),
        h("section", { class: "card", id: "portfolio-summary" }),
        h("div", { class: "page-head", style: "margin-top:14px" }, h("div", { class: "section-heading", id: "accounts-heading", style: "margin:0" }), h("span", { class: "kbd-hint", text: t("keyboard_hint") })),
        h("div", { class: "grid", id: "trading-account-grid" }));
      this.update(new Set(["accounts"]));
      this.staleTimer = setInterval(() => this.update(new Set(["accounts"])), 15000);
    },
    update(topics) {
      if (!topics.has("accounts") && !topics.has("quotes")) return;
      if (!$("#trading-account-grid")) { clearInterval(this.staleTimer); return; }
      const view = state.accounts;
      if (!view) return;
      const byId = new Map((view.accounts || []).map((a) => [a.account_id, a]));
      const metas = view.metas || [];
      const quotes = view.stock_quotes || {};
      const portfolio = view.portfolio || { cash: {}, positions: [] };
      const now = Date.now();

      // KPIs
      const withData = metas.filter((m) => byId.has(m.id)).length;
      const tradingOn = metas.filter((m) => byId.get(m.id) && byId.get(m.id).trading_enabled).length;
      const usdTotal = portfolio.cash && portfolio.cash.USD !== undefined ? portfolio.cash.USD : null;
      $("#account-kpis").replaceChildren(
        kpi(t("usd_cash_total"), fmt.money(usdTotal), t("snapshot_coverage") + ": " + withData + " / " + metas.length),
        kpi(t("portfolio_total_market_value"), fmt.money(portfolio.total_market_value), t("portfolio_total_market_value_help")),
        kpi(t("accounts_trading_on"), tradingOn + " / " + metas.length, t("trading")),
        kpi(t("last_update"), view.generated_at ? fmt.age(view.generated_at) : "—", view.generated_at ? fmt.clock(view.generated_at) : ""));

      // Portfolio
      const cashChips = Object.entries(portfolio.cash || {}).map(([cur, amt]) => h("div", { class: "chip" }, h("div", { class: "chip-label", text: cur }), h("div", { class: "chip-value num", text: fmt.num(amt) })));
      const rows = (portfolio.positions || []).map((p) => h("tr", null,
        h("td", null, h("b", { text: p.symbol })),
        h("td", { class: "right num", text: fmt.num(p.qty) }),
        h("td", { class: "right num", text: p.avg_price === null ? "—" : fmt.price(p.avg_price) }),
        h("td", { class: "right num", text: p.last === null ? "--" : fmt.price(p.last) }),
        h("td", { class: "right num", text: p.market_value === null ? "--" : fmt.num(p.market_value) }),
        h("td", { class: "right num", text: fmt.int(p.account_count) })));
      const coverage = h("div", { class: "card-sub" }, t("snapshot_coverage") + ": ", h("b", { text: portfolio.included_accounts + " / " + portfolio.total_accounts }),
        portfolio.included_accounts < portfolio.total_accounts ? h("div", { class: "warn", text: t("portfolio_incomplete") }) : null);
      let overview = $("#portfolio-overview");
      if (!overview) {
        overview = h("div", { id: "portfolio-overview" });
        $("#portfolio-summary").replaceChildren(overview,
          h("div", { class: "table-scroll", id: "portfolio-positions", tabindex: "0", role: "region", "aria-label": t("portfolio_totals") }));
      }
      overview.replaceChildren(
        h("div", { class: "card-head" }, h("h2", { class: "card-title", text: t("portfolio") }), coverage),
        h("div", { class: "help", text: t("portfolio_summary") }),
        h("div", { class: "section-heading", text: t("cash_holdings") }),
        h("div", { class: "chips" }, cashChips.length ? cashChips : h("em", { text: "—" })),
        h("div", { class: "section-heading", text: t("portfolio_totals") }),
        h("div", { class: "chips" }, h("div", { class: "chip" }, h("div", { class: "chip-label", text: t("portfolio_total_market_value") }),
          h("div", { class: "chip-value num", id: "portfolio-market-value-total", text: fmt.num(portfolio.total_market_value || 0) }))),
        h("div", { class: "help", text: t("portfolio_total_market_value_help") }),
        h("div", { class: "section-heading", text: t("positions") }));
      updateScrollableTable($("#portfolio-positions"), h("table", { class: "data" },
          h("thead", null, h("tr", null, h("th", { text: t("symbol") }), h("th", { class: "right", text: t("qty") }), h("th", { class: "right", title: t("weighted_avg_px"), text: t("avg_px") }),
            h("th", { class: "right", title: t("portfolio_last_help"), text: t("portfolio_last") }), h("th", { class: "right", title: t("portfolio_market_value_help"), text: t("portfolio_market_value") }), h("th", { class: "right", text: t("holding_accounts") }))),
          h("tbody", null, rows.length ? rows : h("tr", null, h("td", { colspan: 6 }, h("em", { text: "—" }))))));

      // Account cards
      const sorted = sortMetas(metas, byId, this.sort, this.security);
      const filtered = sorted.filter((m) => {
        if (!this.showEmpty && !byId.has(m.id)) return false;
        if (!this.filter) return true;
        const hay = [m.id, m.broker, m.machine_alias, m.ip_address, String(m.num_id), m.trading_medium].join(" ").toLowerCase();
        return hay.includes(this.filter);
      });
      $("#accounts-heading").textContent = t("trading_accounts") + " · " + t("account_count", { count: filtered.length });
      const grid = $("#trading-account-grid");
      grid.replaceChildren(...(filtered.length ? filtered.map((m) => accountCard(m, byId.get(m.id), quotes, now)) : [h("div", { class: "empty", text: t("no_data") })]));
    },
  };

  function kpi(label, value, note) {
    return h("div", { class: "kpi" }, h("div", { class: "kpi-label", text: label }), h("div", { class: "kpi-value num", text: value }), note ? h("div", { class: "kpi-note", text: note }) : null);
  }

  function sortMetas(metas, byId, sort, security) {
    const cash = (m) => { const a = byId.get(m.id); return a ? Number(usdCash(a) || 0) : 0; };
    const holding = (m) => { const a = byId.get(m.id); if (!a) return 0; let s = 0; for (const p of a.positions || []) if (String(p.symbol || "").trim().toUpperCase() === security) s += Number(p.qty || 0); return s; };
    const items = metas.slice();
    const missingFirst = (m) => (byId.has(m.id) ? 0 : 1);
    if (sort === SORT_CASH) items.sort((a, b) => missingFirst(a) - missingFirst(b) || cash(b) - cash(a) || a.num_id - b.num_id || a.id.localeCompare(b.id));
    else if (sort === SORT_SECURITY) items.sort((a, b) => missingFirst(a) - missingFirst(b) || holding(b) - holding(a) || cash(b) - cash(a) || a.num_id - b.num_id || a.id.localeCompare(b.id));
    else items.sort((a, b) => a.num_id - b.num_id || a.id.localeCompare(b.id));
    return items;
  }

  function isStale(ts, now) {
    if (!ts) return false;
    let text = String(ts).trim();
    if (!/(?:[zZ]|[+-]\d{2}:?\d{2})$/.test(text)) text += "Z";
    const parsed = Date.parse(text);
    return Number.isFinite(parsed) && now - parsed > 5 * 60 * 1000;
  }

  function accountCard(meta, acct, quotes, now) {
    const metaParts = [t("broker") + ": " + meta.broker, t("trading_medium") + ": " + meta.trading_medium];
    const metaLine = h("div", { class: "meta" });
    metaLine.append(metaParts.join(" · "));
    if (meta.machine_alias) metaLine.append(" · " + t("machine_alias") + ": " + meta.machine_alias);
    if (meta.ip_address) {
      metaLine.append(" · " + t("ip_address") + ": ");
      metaLine.append(meta.monitor ? h("a", { href: "https://" + meta.ip_address, target: "_blank", rel: "noopener noreferrer", text: meta.ip_address }) : meta.ip_address);
    }
    if (meta.broker_id) metaLine.append(" · " + t("broker_id") + "=" + meta.broker_id);
    let pill, cash, tsText, stale = null;
    if (!acct) {
      pill = h("span", { class: "pill unknown", text: t("trading_unknown") });
      cash = "—";
      tsText = t("no_snapshot");
    } else {
      pill = h("span", { class: "pill " + (acct.trading_enabled ? "on" : "off"), text: t(acct.trading_enabled ? "trading_on" : "trading_off") });
      cash = fmt.money(usdCash(acct));
      tsText = acct.ts ? fmt.time(acct.ts) : "";
      if (isStale(acct.ts, now)) stale = h("span", { class: "pill stale", title: t("stale_account_help"), text: t("stale") });
    }
    const extraCash = acct ? Object.entries(acct.cash_by_currency || {}).filter(([c]) => c !== "USD").map(([c, v]) => c + " " + fmt.num(v)).join(" · ") : "";
    const positions = acct ? acct.positions.slice().sort((a, b) => String(a.symbol).localeCompare(String(b.symbol))) : [];
    const posRows = positions.map((p) => {
      const q = quotes[String(p.symbol || "").trim().toUpperCase()];
      const last = q && typeof q.last === "number" ? q.last : null;
      return h("tr", null, h("td", { text: p.symbol }), h("td", { class: "right num", text: fmt.num(p.qty) }),
        h("td", { class: "right num", text: p.avg_price === null || p.avg_price === undefined ? "" : fmt.price(p.avg_price) }),
        h("td", { class: "right num faint", text: last === null ? "--" : fmt.num(last * Number(p.qty || 0)) }));
    });
    const label = "#" + meta.num_id + " - " + meta.id;
    return h("div", { class: "card account-card", dataset: { accountId: meta.id, updatedAt: acct && acct.ts ? acct.ts : "" } },
      h("div", { class: "card-head" }, h("div", { class: "acct", text: label }), h("div", { class: "ts", text: tsText })),
      metaLine,
      h("div", { class: "status-row" }, t("trading") + ":", pill, stale),
      h("div", { class: "cash-line" }, t("cash") + " (USD): ", h("b", { class: "num", text: cash }), extraCash ? h("span", { class: "cash-extra num", text: extraCash }) : null),
      h("div", { class: "table-scroll" }, h("table", { class: "data" },
        h("thead", null, h("tr", null, h("th", { text: t("symbol") }), h("th", { class: "right", text: t("qty") }), h("th", { class: "right", text: t("avg_px") }), h("th", { class: "right", text: t("portfolio_market_value") }))),
        h("tbody", null, posRows.length ? posRows : h("tr", null, h("td", { colspan: 4 }, h("em", { class: "faint", text: t("positions_none") })))))),
      h("div", { class: "account-actions" },
        h("button", { class: "btn sm", type: "button", dataset: { accountOrder: "market", accountId: meta.id }, onclick: () => openAccountOrderModal("market", meta, label) }, t("market_order")),
        h("button", { class: "btn sm green", type: "button", dataset: { accountOrder: "limit", accountId: meta.id }, onclick: () => openAccountOrderModal("limit", meta, label) }, t("limit_order"))));
  }

  // Per-account order dialogs (quick market / limit), posting to the /api/account-orders endpoints.
  function symbolPicker(name, defaultFirst) {
    const symbols = (state.boot.symbols || []).slice().sort();
    const input = h("input", { type: "text", name, autocomplete: "off", spellcheck: "false", placeholder: t("symbol_placeholder") });
    const buttons = symbols.map((s, i) => h("button", { class: "choice-btn", type: "button", "aria-pressed": defaultFirst && i === 0 ? "true" : "false", dataset: { symbol: s }, onclick: (e) => {
      const pressed = e.currentTarget.getAttribute("aria-pressed") === "true";
      buttons.forEach((b) => b.setAttribute("aria-pressed", "false"));
      if (!pressed) e.currentTarget.setAttribute("aria-pressed", "true");
      input.required = !selected();
      input.dispatchEvent(new Event("symbolchange"));
    } }, s));
    const selected = () => { const b = buttons.find((x) => x.getAttribute("aria-pressed") === "true"); return b ? b.dataset.symbol : input.value.trim().toUpperCase(); };
    input.required = !selected();
    input.addEventListener("input", () => input.dispatchEvent(new Event("symbolchange")));
    const el = h("div", null, h("div", { class: "choice-grid", role: "group", style: "margin:6px 0" }, buttons), input, h("div", { class: "help", text: t("account_order_symbol_help") }));
    return { el, input, selected };
  }
  function radioGroup(name, options, checkedValue) {
    const uid = name + "-" + Math.random().toString(36).slice(2, 8);
    const inputs = [];
    const el = h("div", { class: "choice-grid" }, options.map((o) => {
      const id = uid + "-" + String(o.value).replace(/[^a-z0-9]/gi, "");
      const input = h("input", { class: "choice-input", type: "radio", id, name: uid, value: o.value, checked: String(o.value) === String(checkedValue) });
      inputs.push(input);
      return [input, h("label", { class: "choice-btn", for: id, text: o.label })];
    }));
    return { el, value: () => { const c = inputs.find((i) => i.checked); return c ? c.value : ""; }, inputs };
  }

  function openAccountOrderModal(kind, meta, label) {
    const isLimit = kind === "limit";
    const picker = symbolPicker("symbol", true);
    const side = radioGroup("side", [{ value: "BUY", label: t("buy") }, { value: "SELL", label: t("sell") }], "BUY");
    const shares = h("input", { type: "text", inputmode: "numeric", placeholder: "e.g. 100", required: true });
    const price = h("input", { type: "text", inputmode: "decimal", placeholder: "e.g. 123.45" });
    const through = radioGroup("through", [{ value: "", label: t("none") }, ...state.boot.through_market_choices.map((p) => ({ value: p, label: p + "%" }))], "");
    const shareButtons = state.boot.limit_share_shortcuts.map((n) => h("button", { class: "choice-btn", type: "button", onclick: () => { shares.value = String(n); syncShares(); shares.focus(); } }, String(n)));
    const syncShares = () => shareButtons.forEach((b) => b.classList.toggle("active", b.textContent === shares.value.trim()));
    shares.addEventListener("input", syncShares);
    const cashEl = h("b", { class: "num", text: "—" }), lastEl = h("b", { class: "num", text: "—" });
    const message = h("div", { class: "help", role: "status", style: "min-height:1.2em" });
    function renderSummary() {
      const acct = (state.accounts && state.accounts.accounts || []).find((a) => a.account_id === meta.id);
      cashEl.textContent = fmt.money(usdCash(acct));
      lastEl.textContent = lastPriceText(picker.selected(), renderSummary);
    }
    picker.input.addEventListener("symbolchange", renderSummary);
    const form = h("form", { onsubmit: async (e) => {
      e.preventDefault();
      const payload = { account_id: meta.id, symbol: picker.selected(), side: side.value(), shares: shares.value.trim() };
      if (isLimit) { payload.limit_price = price.value.trim(); payload.through_market_pct = through.value(); }
      const controls = $$("input,select,button", form);
      controls.forEach((c) => (c.disabled = true));
      message.className = "help";
      message.textContent = t("submitting");
      try {
        const result = await api(isLimit ? "/api/account-orders/limit" : "/api/account-orders/quick-market", { form: payload });
        if (isLimit && price.value.trim() && result.limit_price != null) price.value = String(result.limit_price);
        message.className = "ok-msg";
        message.textContent = result.message || t("order_submitted");
        toast("ok", result.message || t("order_submitted"));
      } catch (err) {
        message.className = "warn";
        message.textContent = err.message || t("publish_failed");
        if (err instanceof AuthError) showLogin(err.message);
      } finally {
        controls.forEach((c) => (c.disabled = false));
      }
    } },
      h("div", { class: "field" }, h("label", { text: isLimit ? t("limit_symbol") : t("quick_symbol") }), picker.el),
      h("div", { class: "field" }, h("label", { text: t("side") }), side.el),
      h("div", { class: isLimit ? "row" : "" },
        h("div", { class: "field" }, h("label", { text: t("shares") }), shares, isLimit ? h("div", { class: "choice-grid", style: "margin-top:6px" }, shareButtons) : null, isLimit ? h("div", { class: "help", text: t("limit_quick_shares") }) : null),
        isLimit ? h("div", { class: "field" }, h("label", { text: t("limit_price") }), price, h("div", { class: "help", text: t("limit_price_precedence") })) : null),
      isLimit ? h("div", { class: "field" }, h("label", { text: t("through_market_pct") }), through.el) : null,
      h("div", { class: "help" }, t("quick_selected_cash") + ": ", cashEl, " | ", t("quick_selected_last") + ": ", lastEl),
      message,
      h("div", { class: "modal-actions" }, h("button", { class: "btn gray", type: "button", onclick: closeModal }, t("fast_config_cancel")), h("button", { class: "btn " + (isLimit ? "green" : ""), type: "submit" }, isLimit ? t("submit_limit_order") : t("submit_order"))));
    openModal(isLimit ? t("limit_order") : t("quick_market_order"), form, { subtitle: t("account") + ": " + label });
    renderSummary();
    const timer = setInterval(renderSummary, 1000);
    activeModal.onClose = () => clearInterval(timer);
  }

  // ---- Control panel -----------------------------------------------------------------------
  pages["control-panel"] = {
    render(root) {
      const metas = state.boot.accounts;  // sorted by numeric id
      const symbols = state.boot.symbols.slice().sort();
      const accountOptions = () => metas.map((m) => h("option", { value: m.id, text: "#" + m.num_id + " " + m.id + " (" + m.broker + ")" }));
      const symbolOptions = () => symbols.map((s) => h("option", { value: s, text: s }));
      const accountsSnapshot = () => (state.accounts && state.accounts.accounts) || [];
      const flash = h("div", { id: "cp-flash" });

      // --- Market order
      const moAccount = h("select", { id: "account_id", required: true }, accountOptions());
      const moSymbol = h("input", { id: "symbol", type: "text", list: "symbol-suggestions", placeholder: t("symbol_placeholder"), required: true });
      const moSide = radioGroup("market-side", [{ value: "BUY", label: t("buy") }, { value: "SELL", label: t("sell") }], "BUY");
      const moShares = h("input", { id: "shares", type: "text", inputmode: "numeric", placeholder: "e.g. 100" });
      const moDollars = h("input", { id: "dollar_amount", type: "text", inputmode: "decimal", placeholder: "e.g. 2500" });
      const moCash = h("b", { id: "market-selected-cash", class: "num", text: "—" });
      const moMsg = h("div", { class: "warn" });
      const marketForm = h("form", { onsubmit: (e) => submitForm(e, "/api/orders/market", () => ({ account_id: moAccount.value, symbol: moSymbol.value, side: moSide.value(), shares: moShares.value, dollar_amount: moDollars.value }), moMsg) },
        h("div", { class: "row" }, h("div", { class: "field" }, h("label", { for: "account_id", text: t("account") }), moAccount), h("div", { class: "field" }, h("label", { for: "symbol", text: t("symbol") }), moSymbol)),
        h("div", { class: "field" }, h("label", { text: t("side") }), moSide.el),
        h("div", { class: "row" }, h("div", { class: "field" }, h("label", { for: "shares", text: t("shares") }), moShares, h("div", { class: "help", text: t("either_or") })), h("div", { class: "field" }, h("label", { for: "dollar_amount", text: t("dollars") }), moDollars, h("div", { class: "help", text: t("notional_hint") }))),
        h("div", { class: "help" }, t("selected_account_cash") + ": ", moCash), moMsg,
        h("button", { class: "btn", type: "submit" }, t("submit_order")));
      const renderMarketCash = () => { const a = accountsSnapshot().find((x) => x.account_id === moAccount.value); moCash.textContent = a ? fmt.money(a.cash) : "—"; };
      moAccount.addEventListener("change", renderMarketCash);

      // --- Quick market order
      const qAccounts = checkboxGrid("account_ids", metas.map((m, i) => ({ value: m.id, label: "#" + m.num_id, checked: i === 0, title: m.id })));
      const qSymbol = radioGroup("quick-symbol", symbols.map((s) => ({ value: s, label: s })), symbols[0] || "");
      const qSide = radioGroup("quick-side", [{ value: "BUY", label: t("buy") }, { value: "SELL", label: t("sell") }], "BUY");
      const qDollars = h("input", { id: "quick_dollar_amount", type: "text", inputmode: "decimal", placeholder: String(state.boot.quick_default_dollars) });
      const qBalanceLabel = h("span", { text: t("selected_accounts_usd_cash") }), qBalance = h("b", { id: "quick-selected-balance", class: "num", "aria-live": "polite", text: "—" }), qLast = h("b", { id: "quick-selected-last", class: "num", text: "—" });
      const qMsg = h("div", { class: "warn" });
      const quickForm = h("form", { onsubmit: (e) => submitForm(e, "/api/orders/quick", () => ({ account_ids: qAccounts.values(), symbol: qSymbol.value(), side: qSide.value(), dollar_amount: qDollars.value }), qMsg) },
        h("div", { class: "field" }, h("label", { text: t("quick_accounts") }), qAccounts.el),
        h("div", { class: "field" }, h("label", { text: t("quick_symbol") }), qSymbol.el),
        h("div", { class: "field" }, h("label", { text: t("quick_side") }), qSide.el),
        h("div", { class: "field" }, h("label", { for: "quick_dollar_amount", text: t("quick_dollars") }), qDollars),
        h("div", { class: "help" }, qBalanceLabel, ": ", qBalance, " | ", t("quick_selected_last") + ": ", qLast), qMsg,
        h("button", { class: "btn", type: "submit" }, t("submit_order")));
      const renderQuick = () => renderBalances(qAccounts.values(), qSymbol.value(), qSide.value() === "SELL", qBalanceLabel, qBalance, qLast);
      [qAccounts.el, qSymbol.el, qSide.el].forEach((el) => el.addEventListener("change", renderQuick));

      // --- Limit order (with localStorage persistence of ticker/accounts/side)
      const savedAccounts = parseStoredAccounts(storage.get("trading_ui.limit_order.account_ids"));
      const savedSymbol = storage.get("trading_ui.limit_order.symbol");
      const savedSide = storage.get("trading_ui.limit_order.side");
      const lAccounts = checkboxGrid("account_ids", metas.map((m, i) => ({ value: m.id, label: "#" + m.num_id, checked: savedAccounts.length ? savedAccounts.includes(m.id) : i === 0, title: m.id })));
      if (savedAccounts.length && !lAccounts.values().length) lAccounts.inputs[0].checked = true;
      const lSymbol = h("select", { id: "limit_symbol", required: true }, symbolOptions());
      if (savedSymbol && symbols.includes(savedSymbol)) lSymbol.value = savedSymbol;
      const lSide = radioGroup("limit-side", [{ value: "BUY", label: t("buy") }, { value: "SELL", label: t("sell") }], savedSide === "SELL" ? "SELL" : "BUY");
      const lShares = h("input", { id: "limit_shares", type: "text", inputmode: "numeric", placeholder: "e.g. 100", required: true });
      const lShareButtons = state.boot.limit_share_shortcuts.map((n) => h("button", { class: "choice-btn", type: "button", dataset: { limitShares: n }, onclick: () => { lShares.value = String(n); syncLimitShares(); lShares.focus(); } }, String(n)));
      const syncLimitShares = () => lShareButtons.forEach((b) => b.classList.toggle("active", b.dataset.limitShares === lShares.value.trim()));
      lShares.addEventListener("input", syncLimitShares);
      const lPrice = h("input", { id: "limit_price", type: "text", inputmode: "decimal", placeholder: "e.g. 182.50" });
      const lThrough = radioGroup("limit-through", [{ value: "", label: t("none") }, ...state.boot.through_market_choices.map((p) => ({ value: p, label: p + "%" }))], "");
      const lBalanceLabel = h("span", { text: t("selected_accounts_usd_cash") }), lBalance = h("b", { id: "limit-selected-balance", class: "num", text: "—" }), lLast = h("b", { id: "limit-selected-last", class: "num", text: "-" });
      const lMsg = h("div", { class: "warn" });
      const persistLimit = () => { storage.set("trading_ui.limit_order.symbol", lSymbol.value); const sel = lAccounts.values(); if (sel.length) storage.set("trading_ui.limit_order.account_ids", JSON.stringify(sel)); storage.set("trading_ui.limit_order.side", lSide.value()); };
      const limitForm = h("form", { id: "limit-order-form", onsubmit: (e) => { persistLimit(); submitForm(e, "/api/orders/limit", () => ({ account_ids: lAccounts.values(), symbol: lSymbol.value, side: lSide.value(), shares: lShares.value, limit_price: lPrice.value, through_market_pct: lThrough.value() }), lMsg); } },
        h("div", { class: "field" }, h("label", { text: t("limit_accounts") }), lAccounts.el),
        h("div", { class: "row" }, h("div", { class: "field" }, h("label", { for: "limit_symbol", text: t("limit_symbol") }), lSymbol), h("div", { class: "field" }, h("label", { text: t("limit_side") }), lSide.el)),
        h("div", { class: "row" }, h("div", { class: "field" }, h("label", { for: "limit_shares", text: t("shares") }), lShares, h("div", { class: "choice-grid", style: "margin-top:6px", "aria-label": t("limit_quick_shares") }, lShareButtons)), h("div", { class: "field" }, h("label", { for: "limit_price", text: t("limit_price") }), lPrice, h("div", { class: "help", text: t("limit_price_precedence") }))),
        h("div", { class: "field" }, h("label", { text: t("through_market_pct") }), lThrough.el),
        h("div", { class: "help" }, lBalanceLabel, ": ", lBalance, " | ", t("quick_selected_last") + ": ", lLast), lMsg,
        h("button", { class: "btn", type: "submit" }, t("submit_limit_order")));
      const renderLimit = () => renderBalances(lAccounts.values(), lSymbol.value, lSide.value() === "SELL", lBalanceLabel, lBalance, lLast);
      [lAccounts.el, lSide.el].forEach((el) => el.addEventListener("change", () => { persistLimit(); renderLimit(); }));
      lSymbol.addEventListener("change", () => { persistLimit(); renderLimit(); });

      // --- Algorithmic trading
      const modes = [{ value: "E", label: t("mode_e") }, { value: "F", label: t("mode_f") }];
      if (state.boot.smart_selling_available) modes.push({ value: "G", label: t("mode_g") });
      if (state.boot.smart_buying_available) modes.push({ value: "H", label: t("mode_h") });
      const algoMode = radioGroup("algo-mode", modes, "E");
      algoMode.inputs.forEach((i) => {
        i.id = "algo-mode-" + i.value.toLowerCase();
        i.nextSibling.setAttribute("for", i.id);
        const category = ["E", "H"].includes(i.value) ? "buying" : "selling";
        i.nextSibling.append(" ", h("small", { dataset: { algoCategory: category }, text: "(" + t("algo_category_" + category) + ")" }));
      });
      const algoSymbol = h("select", { id: "algo_symbol", required: true }, symbolOptions());
      const algoMsg = h("div", { class: "help", id: "smart-selling-message", role: "status" });
      const algoForm = h("form", { id: "algo-form", onsubmit: (e) => {
        e.preventDefault();
        const mode = algoMode.value();
        if (mode === "G") openSmartModal(algoSymbol.value, algoMsg, false);
        else if (mode === "H") openSmartModal(algoSymbol.value, algoMsg, true);
        else openFastModal(mode, algoSymbol.value, algoMsg);
      } },
        h("div", { class: "row" }, h("div", { class: "field" }, h("label", { text: t("trading_mode") }), algoMode.el), h("div", { class: "field" }, h("label", { for: "algo_symbol", text: t("symbol") }), algoSymbol)),
        h("p", { class: "help", text: t("algo_session_rules") }),
        h("button", { class: "btn green", id: "algo-submit", type: "submit" }, t("algo_submit")));
      const algoStatus = h("div", { id: "algo-status" });
      const smartStatus = h("div", { id: "smart-status" });
      const buyingStatus = h("div", { id: "smart-buying-status" });

      // --- Stop all
      const stopReason = h("input", { id: "stop_reason", type: "text", placeholder: t("stop_reason_placeholder") });
      const stopMsg = h("div", { class: "warn" });
      const stopForm = h("form", { onsubmit: async (e) => { e.preventDefault(); if (!(await confirmDialog(t("confirm_stop_all")))) return; submitForm(e, "/api/algo/stop", () => ({ reason: stopReason.value }), stopMsg, true); } },
        h("p", { class: "help", text: t("algo_stop_help") }),
        h("div", { class: "field" }, h("label", { for: "stop_reason", text: t("stop_reason") }), stopReason), stopMsg,
        h("button", { class: "btn red", type: "submit" }, t("algo_stop")));

      // --- Cancel open orders
      const cAll = h("input", { class: "choice-input", type: "checkbox", id: "cancel-acct-all", value: "__ALL__", checked: true });
      const cAccounts = checkboxGrid("account_ids", metas.map((m) => ({ value: m.id, label: "#" + m.num_id + " " + m.id, checked: false, title: [m.id, m.broker, m.machine_alias, m.ip_address].filter(Boolean).join(" | ") })));
      cAll.addEventListener("change", () => { if (cAll.checked) cAccounts.inputs.forEach((i) => (i.checked = false)); });
      cAccounts.inputs.forEach((i) => i.addEventListener("change", () => { if (i.checked) cAll.checked = false; }));
      const cSymbol = h("input", { id: "cancel_symbol", type: "text", list: "symbol-suggestions", placeholder: t("cancel_symbol_placeholder") });
      const cMsg = h("div", { class: "warn" });
      const cancelForm = h("form", { id: "cancel-open-orders-form", onsubmit: async (e) => { e.preventDefault(); const ids = cAll.checked ? ["__ALL__"] : cAccounts.values(); if (cAll.checked && !(await confirmDialog(t("confirm_cancel_all")))) return; submitForm(e, "/api/orders/cancel", () => ({ account_ids: ids, symbol: cSymbol.value }), cMsg, true); } },
        h("div", { class: "row" }, h("div", { class: "field" }, h("label", { text: t("cancel_accounts") }), h("div", { class: "choice-grid" }, cAll, h("label", { class: "choice-btn", for: "cancel-acct-all", text: t("all_accounts") }), cAccounts.el.childNodes.length ? Array.from(cAccounts.el.childNodes) : null)), h("div", { class: "field" }, h("label", { for: "cancel_symbol", text: t("cancel_symbol") }), cSymbol)),
        h("div", { class: "help", text: t("cancel_orders_help") }), cMsg,
        h("button", { class: "btn", type: "submit" }, t("cancel_open_orders")));

      const recent = h("div", { id: "recent-commands" });

      root.append(
        h("div", { class: "page-head" }, h("h1", { class: "page-title", text: t("tab_control") })),
        flash,
        h("div", { class: "grid-2" },
          h("div", { class: "col" },
            card(t("market_order"), marketForm),
            card(t("quick_market_order"), quickForm),
            card(t("algo_stop_title"), stopForm),
            card(t("recent_commands"), recent)),
          h("div", { class: "col" },
            card(t("limit_order"), limitForm),
            card(t("algo_title"), [algoForm, h("div", { class: "divider" }), h("div", { class: "section-heading", text: t("active_algo") }), algoStatus,
              state.boot.smart_selling_available ? [h("div", { class: "divider" }), h("div", { class: "section-heading", text: t("smart_status_title") }), smartStatus] : null,
              state.boot.smart_buying_available ? [h("div", { class: "divider" }), h("div", { class: "section-heading", text: t("smart_buying_status_title") }), buyingStatus] : null,
              h("div", { class: "help", style: "margin-top:8px", text: t("smart_sessions_filter_note") })]),
            card(t("cancel_orders_title"), cancelForm))),
        h("datalist", { id: "symbol-suggestions" }, symbolOptions()));

      this.renderers = { renderMarketCash, renderQuick, renderLimit };
      this.update(new Set(["accounts", "quotes", "smart", "algo", "commands"]));
    },
    update(topics) {
      if (!$("#algo-form")) return;
      if (topics.has("accounts") || topics.has("quotes")) { this.renderers.renderMarketCash(); this.renderers.renderQuick(); this.renderers.renderLimit(); }
      if (topics.has("smart") || topics.has("algo")) renderSmartStatus($("#smart-status"), $("#algo-status"), $("#smart-buying-status"));
      if (topics.has("commands")) renderRecentCommands($("#recent-commands"));
    },
  };

  function card(title, body) {
    return h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", { class: "card-title", text: title })), body);
  }
  function checkboxGrid(name, items) {
    const uid = name + "-" + Math.random().toString(36).slice(2, 8);
    const inputs = [];
    const el = h("div", { class: "choice-grid" }, items.map((it, i) => {
      const id = uid + "-" + i;
      const input = h("input", { class: "choice-input", type: "checkbox", id, name, value: it.value, checked: !!it.checked });
      inputs.push(input);
      return [input, h("label", { class: "choice-btn", for: id, title: it.title || "", text: it.label })];
    }));
    return { el, inputs, values: () => inputs.filter((i) => i.checked).map((i) => i.value) };
  }
  function parseStoredAccounts(raw) {
    if (!raw) return [];
    try { const parsed = JSON.parse(raw); if (Array.isArray(parsed)) return parsed.map((v) => String(v || "").trim()).filter(Boolean); } catch (e) { /* legacy */ }
    return String(raw).split(",").map((v) => v.trim()).filter(Boolean);
  }
  function renderBalances(accountIds, symbol, selling, labelEl, balanceEl, lastEl) {
    symbol = String(symbol || "").trim().toUpperCase();
    labelEl.textContent = selling ? t("selected_accounts_owned_shares") + (symbol ? " (" + symbol + ")" : "") : t("selected_accounts_usd_cash");
    const accounts = (state.accounts && state.accounts.accounts) || [];
    const parts = accountIds.map((id) => {
      const a = accounts.find((x) => x.account_id === id);
      const label = a && a.account_num_id !== null && a.account_num_id !== undefined ? "#" + a.account_num_id : id;
      return label + ": " + formatAccountBalance(a, selling, symbol);
    });
    balanceEl.textContent = parts.length ? parts.join(", ") : "—";
    if (lastEl) lastEl.textContent = lastPriceText(symbol, () => renderBalances(accountIds, symbol, selling, labelEl, balanceEl, lastEl));
  }

  async function submitForm(e, url, payloadFn, msgEl, showToast = true) {
    e.preventDefault();
    const form = e.target.closest ? e.target.closest("form") : e.target;
    const controls = form ? $$("input,select,button", form) : [];
    controls.forEach((c) => (c.disabled = true));
    msgEl.className = "help";
    msgEl.textContent = t("submitting");
    try {
      const result = await api(url, { json: payloadFn() });
      msgEl.className = "ok-msg";
      msgEl.textContent = result.message || t("order_submitted");
      if (showToast) toast("ok", result.message || t("order_submitted"));
      if (result.warning) toast("err", result.warning, 10000);
      return result;
    } catch (err) {
      msgEl.className = "warn";
      msgEl.textContent = err.message || t("error_generic");
      toast("err", err.message || t("error_generic"));
      if (err instanceof AuthError) showLogin(err.message);
      return null;
    } finally {
      controls.forEach((c) => (c.disabled = false));
    }
  }

  function renderRecentCommands(root) {
    if (!root) return;
    const commands = (state.commands && state.commands.commands) || [];
    if (!commands.length) { root.replaceChildren(h("div", { class: "empty", text: t("recent_commands_empty") })); return; }
    root.replaceChildren(h("div", { class: "table-scroll" }, h("table", { class: "data" },
      h("thead", null, h("tr", null, h("th", { text: t("time") }), h("th", { text: t("type") }), h("th", { text: t("account") }), h("th", { text: t("details") }), h("th", { text: t("status") }))),
      h("tbody", null, commands.slice(0, 25).map((c) => {
        const cmd = c.command || {};
        const details = [cmd.symbol, cmd.side, cmd.qty_shares !== undefined ? fmt.int(cmd.qty_shares) : null, cmd.notional_usd !== undefined ? fmt.money(cmd.notional_usd) : null, cmd.limit_price !== undefined ? "@" + fmt.price(cmd.limit_price) : null, cmd.trading_mode ? t("algo_mode_label") + " " + cmd.trading_mode : null, cmd.trading_enabled !== undefined ? (cmd.trading_enabled ? t("turn_on") : t("turn_off")) : null, cmd.source_currency ? cmd.source_currency + "→" + cmd.target_currency + " " + fmt.num(cmd.source_amount) : null, cmd.trading_date ? cmd.trading_date + " · " + t("account_count", { count: (cmd.account_ids || []).length }) : null].filter(Boolean).join(" ");
        return h("tr", null, h("td", { class: "num", text: fmt.clock(c.at) }), h("td", null, h("code", { text: cmd.type || "" })), h("td", { text: cmd.account_id || (Array.isArray(cmd.account_ids) ? cmd.account_ids.join(", ") : c.key) }), h("td", { text: details, title: cmd.command_id || "" }), h("td", null, h("span", { class: "pill " + (c.status === "failed" ? "off" : c.status === "queued" ? "unknown" : "on"), text: c.status === "failed" ? c.error || c.status : t("published") })));
      })))));
  }

  const SMART_STATE_KEYS = { IDLE: "idle", SCHEDULED: "scheduled", RUNNING: "running", HOLD: "halted", STOPPING: "stopping", STOPPED: "stopped", COMPLETED: "completed", DEADLINE: "expired", SUBMIT_PENDING: "submit_pending", LIVE: "live", CANCEL_PENDING: "cancel_pending", RECONCILE: "reconcile", EXPIRED: "expired", HALTED: "halted" };
  function smartStateLabel(stateName) { const key = SMART_STATE_KEYS[stateName]; return key ? t("smart_state_" + key) : String(stateName || "—"); }

  const stopRequests = new Set();
  // Sessions started today (browser-local date) plus every active session, as the trading UI shows them.
  function visibleParents(parents) {
    const today = new Date().toDateString();
    return (parents || []).filter((p) => p.active || new Date(p.start).toDateString() === today);
  }
  function renderSmartStatus(smartRoot, algoRoot, buyingRoot) {
    if (algoRoot) {
      const fast = (state.algo && state.algo.fast) || [];
      if (!fast.length) {
        algoRoot.replaceChildren(h("div", { class: "empty", text: t("no_active_algo") }));
      } else {
        algoRoot.replaceChildren(...fast.map((s) => {
          const cmd = s.command || {};
          const cycle = s.last_cycle || {};
          const target = cmd.fast_trading_total_target_position;
          const position = target !== undefined && target !== null
            ? h("div", { class: "status-meta", text: t("fast_target_position_status", { current: cycle.total_position === null || cycle.total_position === undefined ? "—" : fmt.qty(cycle.total_position), target: fmt.int(target) }) })
            : null;
          return h("div", { class: "status-item" },
            h("div", null,
              h("div", { class: "status-title" }, h("span", { class: "pill info", text: cmd.trading_mode || "" }), " ", cmd.symbol || "", " ", s.stopping ? h("span", { class: "pill stale", text: t("smart_state_stopping") }) : h("span", { class: "pill on", text: t("smart_state_running") }), cmd.fast_trading_test_mode ? h("span", { class: "pill purple", text: t("test_mode_tag") }) : null),
              h("div", { class: "status-meta" }, t("algo_running_since") + " " + fmt.clock(s.started_at) + " · " + t("algo_cycles") + " " + s.cycles + " · " + t("algo_published") + " " + s.published_commands + " · " + t("fast_accounts") + ": " + (cmd.fast_trading_account_ids || []).join(", ")),
              h("div", { class: "status-meta" }, t("fast_cycle_result") + ": " + (cycle.reason ? algoReason(cycle.reason) : (cycle.commands ? cycle.commands + " cmd" : "—")) + (cycle.limit_price ? " @" + fmt.price(cycle.limit_price) : "") + (cycle.total_resting_qty ? " · " + t("fast_target_percentage").split(" (")[0] + " " + fmt.int(cycle.target_quantity) + "/" + fmt.int(cycle.total_resting_qty) : "")),
              position,
              h("div", { class: "status-meta faint mono", text: t("fast_session_id") + ": " + (s.display_id || s.session_id || "") })));
        }));
      }
    }
    const smart = state.smart || (state.algo && state.algo.smart);
    if (smartRoot) renderParentsTable(smartRoot, visibleParents(smart && smart.parents), false);
    const buying = state.algo && state.algo.buying;
    if (buyingRoot) renderParentsTable(buyingRoot, visibleParents(buying && buying.parents), true);
  }

  function renderParentsTable(root, parents, buying) {
    if (!parents.length) { root.replaceChildren(h("div", { class: "empty", text: t(buying ? "smart_buying_status_empty" : "smart_status_empty") })); return; }
    let summaries = $(".smart-session-summaries", root);
    let scroll = $(".table-scroll", root);
    if (!scroll) {
      summaries = h("div", { class: "smart-session-summaries", "aria-live": "polite" });
      scroll = h("div", { class: "table-scroll", tabindex: "0", role: "region", "aria-label": t(buying ? "smart_buying_status_title" : "smart_status_title") });
      root.replaceChildren(summaries, scroll);
    }
    const active = parents.filter((p) => p.active);
    const signature = JSON.stringify([state.lang, active.map((p) => [p.parent_id, p.display_id, p.symbol, p.state, p.reason, p.market_state, p.market_reason])]);
    if (summaries.dataset.signature !== signature) {
      summaries.replaceChildren(...active.map((p) => h("div", { class: "smart-session-summary", dataset: { parentId: p.parent_id } },
        h("div", { class: "status-title" }, p.display_id || p.symbol, " · ", smartStateLabel(p.state)),
        h("div", { class: "status-meta", text: [algoReason(p.reason), p.market_state !== "TRADING" ? p.market_reason : ""].filter(Boolean).join(" · ") }))));
      summaries.dataset.signature = signature;
    }
    const base = buying ? "/api/smart-buying/" : "/api/smart-selling/";
    const rows = parents.map((p) => {
      const childState = p.active_order && p.active_order.state;
      const marketKey = String(p.market_state || "").toLowerCase();
      const stateText = smartStateLabel(p.state) + (childState ? " / " + smartStateLabel(childState) : "") + (state.T["smart_market_" + marketKey] ? " / " + t("smart_market_" + marketKey) : "");
      const reason = [algoReason(p.reason), p.market_state !== "TRADING" ? p.market_reason : ""].filter(Boolean).join(" · ") || "—";
      const accountLabel = Array.isArray(p.account_ids) ? p.account_ids.join(", ") : String(p.account_id || "");
      const total = Number(p.quantity), filled = Number(p.filled);
      const pct = Number.isFinite(total) && total > 0 && Number.isFinite(filled) ? Math.min(100, Math.max(0, (filled / total) * 100)) : 0;
      const terminal = ["STOPPING", "STOPPED", "COMPLETED", "DEADLINE", "EXPIRED"].includes(p.state);
      const stopBtn = h("button", { class: "btn sm red", type: "button", "aria-label": t("smart_stop") + " " + accountLabel + " " + p.symbol, disabled: stopRequests.has(p.parent_id) || terminal, onclick: async () => {
        stopBtn.disabled = true; stopRequests.add(p.parent_id);
        try { await api(base + encodeURIComponent(p.parent_id) + "/stop", { method: "POST" }); toast("info", t("smart_stopping")); }
        catch (err) { toast("err", t("smart_request_failed") + " " + err.message); if (err instanceof AuthError) showLogin(err.message); }
        finally { stopRequests.delete(p.parent_id); refreshAlgo(); }
      } }, t("smart_stop"));
      const bound = buying ? p.maximum_price : p.minimum_price;
      const perAccount = Object.entries(p.accounts || {}).map(([id, a]) => {
        let text = id + ": " + fmt.decimal(a.filled) + "/" + fmt.decimal(a.quantity);
        if (buying) text += " (" + t("smart_cash_budget") + " " + fmt.money(a.spent) + " / " + fmt.money(a.cash_budget) + ")";
        else if (a.average_cost !== null && a.average_cost !== undefined) text += " (" + t("smart_average_cost") + " " + fmt.decimal(a.average_cost) + ")";
        return text;
      }).join(", ");
      const details = h("details", { dataset: { parentId: p.parent_id } }, h("summary", { text: t("details") }), h("dl", { class: "kv" },
        h("dt", { text: t("fast_session_id") }), h("dd", { class:"mono", text:p.display_id || p.parent_id }),
        h("dt", { text: t("smart_mode") }), h("dd", { text: t("smart_" + p.mode) }),
        h("dt", { text: t("smart_credit") }), h("dd", { text: fmt.decimal(p.free_credit) }),
        buying ? null : [h("dt", { text: t("smart_child_cap") }), h("dd", { text: p.disable_child_cap ? t("smart_child_cap_disabled") : fmt.decimal(p.child_cap) })],
        h("dt", { text: t("smart_started_at") }), h("dd", { text: fmt.time(p.start) }),
        h("dt", { text: t("smart_deadline_at") }), h("dd", { text: p.deadline ? fmt.time(p.deadline) : t("smart_no_deadline") }),
        h("dt", { text: t(buying ? "smart_maximum_price_short" : "smart_minimum_price_short") }), h("dd", { text: bound ? fmt.decimal(bound) : "—" }),
        h("dt", { text: t("smart_accounts_detail") }), h("dd", { text: perAccount })));
      return h("tr", null,
        h("td", { class: "smart-account-label" }, h("b", { text: accountLabel }), " / ", p.symbol, details),
        h("td", { class: "right num", text: fmt.decimal(p.quantity) }),
        h("td", { class: "right num" }, fmt.decimal(p.filled), h("div", { class: "progress", title: pct.toFixed(1) + "%" }, h("span", { style: "width:" + pct.toFixed(1) + "%" }))),
        h("td", { class: "right num", text: fmt.decimal(p.remaining) }),
        h("td", { class: "right num", text: fmt.decimal(p.reserved) }),
        h("td", { text: stateText }),
        h("td", { class: "smart-reason", text: reason }),
        h("td", null, stopBtn));
    });
    updateScrollableTable(scroll, h("table", { class: "data", id: buying ? "smart-buying-status-table" : "smart-status-table" },
      h("thead", null, h("tr", null, h("th", { text: t("account") + " / " + t("symbol") }), h("th", { class: "right", text: t(buying ? "smart_target" : "smart_inventory") }), h("th", { class: "right", text: t("smart_filled") }), h("th", { class: "right", text: t("smart_remaining") }), h("th", { class: "right", text: t("smart_reserved") }), h("th", { text: t("smart_state") }), h("th", { text: t("smart_reason") }), h("th", { text: t("smart_stop") }))),
      h("tbody", { id: buying ? "smart-buying-status-rows" : "smart-status-rows" }, rows)));
  }
  async function refreshAlgo() {
    try { state.algo = await api("/api/algo"); state.smart = state.algo.smart; queueRender("algo"); queueRender("smart"); } catch (e) { if (e instanceof AuthError) showLogin(e.message); }
  }

  function accountChecklist(dataAttr, balanceFn) {
    const metas = state.boot.accounts;
    const rows = metas.map((m) => {
      const input = h("input", { type: "checkbox", dataset: { [dataAttr]: m.id } });
      const bal = h("b", { class: "bal", dataset: { [dataAttr + "Balance"]: m.id }, text: "—" });
      return h("label", { class: "check-row" }, input, h("span", null, "#" + m.num_id + " " + m.id + " (" + m.broker + ") ", bal));
    });
    const el = h("div", { class: "check-list" }, rows);
    return { el, selected: () => $$("input:checked", el).map((i) => i.dataset[dataAttr]), render: () => {
      $$(".check-row", el).forEach(row => {
        const input = $("input", row), balance = $(".bal", row);
        const account = ((state.accounts && state.accounts.accounts) || []).find(a => a.account_id === input.dataset[dataAttr]);
        const timestamp = account && account.ts ? Date.parse(account.ts) : NaN;
        const age = Date.now() - timestamp;
        const offline = !Number.isFinite(age) || age > 300000 || age < -30000;
        // Keep an already selected account explicit: the server rejects the whole start if it
        // goes offline, rather than silently running the session on a smaller account set.
        input.disabled = offline && !input.checked;
        balance.textContent = offline ? t("algo_account_offline") : balanceFn(account);
      });
    } };
  }

  function openFastModal(mode, symbol, msgEl) {
    const selling = mode === "F";
    const price = h("input", { id: "fast-price-limit", type: "number", min: "0.01", step: "0.01", inputmode: "decimal" });
    const accounts = accountChecklist("fastAccountId", (a) => "— " + (selling ? t("owned_shares") + " (" + symbol + ")" : t("usd_cash")) + ": " + formatAccountBalance(a, selling, symbol));
    const levels = radioGroup("fast_book_levels", state.boot.fast_trading.book_level_choices.map((v) => ({ value: v, label: String(v) })), "");
    const target = radioGroup("fast_target_percentage", state.boot.fast_trading.target_percentage_choices.map((v) => ({ value: v, label: v + "%" })), "");
    const cycle = radioGroup("fast_cycle_seconds", state.boot.fast_trading.cycle_seconds_choices.map((v) => ({ value: v, label: String(v) })), "");
    const testMode = h("input", { type: "checkbox", id: "fast-test-mode" });
    const targetPosition = h("input", { id: "fast-total-target-position", type: "number", min: "0", step: "1", inputmode: "numeric" });
    const error = h("div", { class: "warn", role: "alert" });
    const body = h("div", null,
      h("div", { class: "field" }, h("label", { for: "fast-price-limit", text: selling ? t("fast_sell_price_limit") : t("fast_buy_price_limit") }), price),
      h("div", { class: "field", style: "margin-top:10px" }, h("label", { text: t("fast_accounts") }), accounts.el),
      h("div", { class: "field", style: "margin-top:10px" }, h("label", { text: t("fast_book_levels") }), levels.el),
      h("div", { class: "field", style: "margin-top:10px" }, h("label", { text: t("fast_target_percentage") }), target.el, h("div", { class: "help", text: t("fast_target_percentage_help") })),
      h("div", { class: "field", style: "margin-top:10px" }, h("label", { text: t("fast_cycle_seconds") }), cycle.el, h("div", { class: "help", text: t("fast_cycle_seconds_help") })),
      h("div", { class: "field", style: "margin-top:10px" }, h("label", { for: "fast-total-target-position", text: t("fast_total_target_position") }), targetPosition,
        h("div", { class: "help", text: t(selling ? "fast_total_target_position_sell_help" : "fast_total_target_position_buy_help") })),
      h("label", { class: "check-row", style: "margin-top:10px" }, testMode, h("span", { text: t("fast_test_mode") })),
      error);
    const submit = h("button", { class: "btn green", type: "button", onclick: async () => {
      const priceLimit = Number(price.value);
      const accountIds = accounts.selected();
      if (!Number.isFinite(priceLimit) || priceLimit <= 0) return (error.textContent = t("fast_price_limit_positive"));
      if (!accountIds.length) return (error.textContent = t("fast_accounts_required"));
      if (!levels.value()) return (error.textContent = t("fast_book_levels_invalid"));
      if (!target.value()) return (error.textContent = t("fast_target_percentage_invalid"));
      if (!cycle.value()) return (error.textContent = t("fast_cycle_seconds_invalid"));
      const targetText = targetPosition.value.trim();
      if (targetText && (!/^\d+$/.test(targetText) || Number(targetText) > Number.MAX_SAFE_INTEGER)) return (error.textContent = t("fast_total_target_position_invalid"));
      error.textContent = "";
      submit.disabled = true;
      try {
        const config = { price_limit: priceLimit, account_ids: accountIds, book_levels: Number(levels.value()), target_percentage: Number(target.value()), cycle_seconds: Number(cycle.value()), test_mode: testMode.checked };
        if (targetText) config.total_target_position = Number(targetText);
        const result = await api("/api/submit-algo", { json: { trading_mode: mode, symbol, fast_trading_config: config } });
        msgEl.textContent = t("algo_submitted") + " " + (result.display_id || result.strategy_key) + ". " + t("published_cmd") + "=" + result.command.command_id;
        toast("ok", t("algo_submitted") + " " + (result.display_id || result.strategy_key));
        closeModal();
        refreshAlgo();
      } catch (err) {
        error.textContent = err.message;
        if (err instanceof AuthError) showLogin(err.message);
      } finally { submit.disabled = false; }
    } }, t("fast_config_submit"));
    openModal(t("fast_config_title"), body, { subtitle: t("fast_selected_mode") + ": " + t(mode === "E" ? "mode_e" : "mode_f") + " · " + t("symbol") + ": " + symbol, actions: [h("button", { class: "btn gray", type: "button", onclick: closeModal }, t("fast_config_cancel")), submit] });
    accounts.render();
    const timer = setInterval(accounts.render, 1000);
    activeModal.onClose = () => clearInterval(timer);
  }

  function localDateTime(date) {
    const pad = (v) => String(v).padStart(2, "0");
    return date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate()) + "T" + pad(date.getHours()) + ":" + pad(date.getMinutes()) + ":" + pad(date.getSeconds());
  }

  // Smart selling (G) and Smart buying (H) share scheduling, submission and status; buying sizes each
  // account from its available USD cash and bounds prices from above.
  function availableUsdAccount(account) {
    if (!account) return account;
    const balances = account.available_cash_by_currency || {};
    const key = Object.keys(balances).find((value) => value.trim().toUpperCase() === "USD");
    return key === undefined ? account : Object.assign({}, account, { cash_by_currency: { USD: balances[key] } });
  }

  function openSmartModal(symbol, msgEl, buying) {
    const prefix = buying ? "smart-buying" : "smart";
    const accounts = accountChecklist(buying ? "smartBuyingAccountId" : "smartAccountId", buying
      ? (a) => "— " + t("smart_buying_cash") + ": " + formatAccountBalance(availableUsdAccount(a), false, "")
      : (a) => "— " + t("owned_shares") + " (" + symbol + "): " + formatAccountBalance(a, true, symbol));
    const startMode = radioGroup(prefix + "_start_mode", [{ value: "now", label: t("smart_start_now") }, { value: "scheduled", label: t("smart_start_scheduled") }], "now");
    const deadlineMode = radioGroup(prefix + "_deadline_mode", [{ value: "none", label: t("smart_no_deadline") }, { value: "scheduled", label: t("smart_set_deadline") }], "scheduled");
    const now = new Date();
    const start = h("input", { id: prefix + "-start", type: "datetime-local", step: "1", value: localDateTime(now), disabled: true, hidden: true });
    const deadline = h("input", { id: prefix + "-deadline", type: "datetime-local", step: "1", value: localDateTime(new Date(now.getTime() + 2 * 3600 * 1000)) });
    const updateTimes = () => { start.disabled = start.hidden = startMode.value() === "now"; deadline.disabled = deadline.hidden = deadlineMode.value() === "none"; };
    startMode.el.addEventListener("change", updateTimes); deadlineMode.el.addEventListener("change", updateTimes);
    const mode = h("select", { id: prefix + "-mode" }, h("option", { value: "patient", text: t("smart_patient") }), h("option", { value: "normal", text: t("smart_normal"), selected: true }), h("option", { value: "urgent", text: t("smart_urgent") }),
      buying ? null : h("option", { value: "ultra", text: t("smart_ultra") }));
    const boundKey = buying ? "maximum" : "minimum";
    const priceBound = h("input", { id: prefix + "-" + boundKey + "-price", type: "number", min: "0", step: "any", inputmode: "decimal" });
    const disableChildCap = buying ? null : h("input", { id: "smart-disable-child-cap", type: "checkbox", "aria-describedby": "smart-child-cap-help" });
    const error = h("div", { class: "warn", role: "alert" });
    const body = h("div", null,
      h("p", { class: "help", text: t(buying ? "smart_buying_description" : "smart_description") }),
      h("div", { class: "field" }, h("label", { text: t("fast_accounts") }), accounts.el),
      h("div", { class: "row", style: "margin-top:10px" },
        h("div", { class: "field" }, h("label", { text: t("smart_start") }), startMode.el, start),
        h("div", { class: "field" }, h("label", { text: t("smart_deadline") }), deadlineMode.el, deadline)),
      h("p", { class: "help", text: t(buying ? "smart_buying_local_time" : "smart_local_time") }),
      h("div", { class: "row" }, h("div", { class: "field" }, h("label", { for: prefix + "-mode", text: t("smart_mode") }), mode), h("div", { class: "field" }, h("label", { for: priceBound.id, text: t("smart_" + boundKey + "_price") }), priceBound)),
      buying ? null : [h("label", { class: "check-row", style: "margin-top:10px" }, disableChildCap, h("span", { text: t("smart_disable_child_cap") })), h("p", { class: "help", id: "smart-child-cap-help", text: t("smart_child_cap_help") })],
      h("p", { class: "help", text: t(buying ? "smart_buying_pov_help" : "smart_completion_help") }),
      error);
    const submit = h("button", { class: "btn green", type: "button", onclick: async () => {
      const accountIds = accounts.selected();
      const startDate = startMode.value() === "now" ? new Date() : new Date(start.value);
      const deadlineDate = deadlineMode.value() === "none" ? null : new Date(deadline.value);
      const bound = priceBound.value.trim() ? Number(priceBound.value) : null;
      if (!accountIds.length) return (error.textContent = t("smart_invalid_account"));
      if (!symbol) return (error.textContent = t("invalid_symbol"));
      if (!Number.isFinite(startDate.getTime()) || (deadlineDate !== null && (!Number.isFinite(deadlineDate.getTime()) || deadlineDate <= startDate || deadlineDate.getTime() <= Date.now()))) return (error.textContent = t("smart_invalid_times"));
      if (bound !== null && (!Number.isFinite(bound) || bound <= 0)) return (error.textContent = t("smart_invalid_price"));
      error.textContent = "";
      submit.disabled = true;
      // The server sizes the session (all sellable inventory, or available cash at the starting ask)
      // and fixes the participation rate; quantity and imminent_event have no UI controls.
      const payload = { account_ids: accountIds, symbol, mode: mode.value, start_time: startMode.value() === "now" ? null : startDate.toISOString(), deadline: deadlineDate === null ? null : deadlineDate.toISOString() };
      payload[boundKey + "_price"] = bound;
      if (disableChildCap) payload.disable_child_cap = disableChildCap.checked;
      try {
        await api(buying ? "/api/smart-buying" : "/api/smart-selling", { json: payload });
        msgEl.textContent = t(buying ? "smart_buying_started" : "smart_started");
        toast("ok", t(buying ? "smart_buying_started" : "smart_started"));
        closeModal();
        refreshAlgo();
      } catch (err) {
        error.textContent = t("smart_request_failed") + " " + err.message;
        if (err instanceof AuthError) showLogin(err.message);
      } finally { submit.disabled = false; }
    } }, t(buying ? "smart_buying_submit" : "smart_submit"));
    openModal(t(buying ? "smart_buying_config_title" : "smart_config_title"), body, { subtitle: t("symbol") + ": " + symbol, actions: [h("button", { class: "btn gray", type: "button", onclick: closeModal }, t("fast_config_cancel")), submit] });
    if (!state.boot.accounts.length) { submit.disabled = true; error.textContent = t("smart_no_accounts"); }
    updateTimes();
    accounts.render();
    const timer = setInterval(accounts.render, 1000);
    activeModal.onClose = () => clearInterval(timer);
  }

  // ---- Market data (lmh only) -------------------------------------------------------------------
  // Five-minute windows of market volume, our own traded volume and an estimated VWAP for one
  // publisher symbol, plus the live quote table for every market-data symbol.
  pages["working-orders"] = {
    render(root) {
      this.root = root; this.closed = false; this.pending = false;
      root.append(h("div", {class:"page-head"}, h("h1", {class:"page-title",text:t("wo_title")})),
        h("p", {class:"help",text:t("wo_note")}), h("div", {id:"wo-status",class:"warn",role:"status"}));
      for (const group of ["manual", "algo", "unknown"]) root.append(
        h("section", {class:"card",style:"margin-bottom:var(--gap)"},
          h("h2", {class:"card-title",text:t("wo_"+group)}), h("div", {class:"table-scroll",id:"wo-"+group})));
      this.poll(); this.timer = setInterval(() => this.poll(), 2000);
    },
    destroy() { this.closed = true; clearInterval(this.timer); },
    async poll() {
      if (this.pending || this.closed) return;
      this.pending = true;
      try {
        const data = await api("/api/working-orders");
        if (this.closed) return;
        $("#wo-status",this.root).textContent = data.feed.state === "live" ? "" : t("wo_feed_wait");
        for (const group of ["manual", "algo", "unknown"]) {
          const rows = data.orders.filter(o => group === "manual" ? o.origin.startsWith("manual_") : o.origin === group);
          const table = h("table", {class:"data"}, h("thead", null, h("tr", null,
            ...["wo_account","symbol","wo_side","wo_quantity","wo_filled","wo_price","wo_state","wo_origin","wo_order_id","wo_action"].map(k=>h("th",{text:t(k)})))),
            h("tbody", null, rows.map(o => {
              const button = h("button", {class:"btn danger",disabled:!o.can_cancel,text:t("wo_cancel"),onclick:async()=>{
                if (!(await confirmDialog(t("wo_confirm") + " " + o.account_id + " / " + o.order_id))) return;
                button.disabled = true;
                try { const result = await api("/api/orders/cancel",{json:{account_ids:[o.account_id],order_id:o.order_id}}); toast("info",result.message || t("wo_pending")); }
                catch(e) { toast("err",e.message); }
                this.poll();
              }});
              return h("tr", null, ...[o.account_id,o.symbol,o.side,fmt.qty(o.quantity),fmt.qty(o.filled_quantity),fmt.price(o.limit_price),
                o.status + (o.cancel_state ? " / " + t("wo_pending") + " (" + o.cancel_state + ")" : ""),
                o.session_label || t("wo_"+o.origin),o.order_id].map(value=>h("td",{text:value})),
                h("td",null,group === "manual" ? button : "—"));
            })));
          if (!rows.length) table.append(h("caption",{text:t("wo_empty")}));
          updateScrollableTable($("#wo-"+group,this.root),table);
        }
      } catch(e) { if (!this.closed) $("#wo-status",this.root).textContent=e.message; }
      finally { this.pending = false; }
    }
  };

  pages["market-data"] = {
    render(root) {
      const symbols = state.boot.market_data_symbols || [];
      const saved = storage.get("ktrader.market_data.symbol");
      const select = h("select", { id: "md-symbol", disabled: !symbols.length }, symbols.map((sym) => h("option", { value: sym, text: sym })));
      if (saved && symbols.includes(saved)) select.value = saved;
      select.addEventListener("change", () => { storage.set("ktrader.market_data.symbol", select.value); this.showEmpty(t("md_loading")); this.poll(); });
      const headCell = (key, attrs) => h("th", Object.assign({ scope: "col" }, attrs || {}), t(key));
      root.append(
        h("div", { class: "page-head" },
          h("div", null, h("h1", { class: "page-title", text: t("md_title") }), h("div", { class: "help", text: t("md_window_note") })),
          h("div", { class: "toolbar" }, h("div", { class: "field" }, h("label", { for: "md-symbol", text: t("symbol") }), select))),
        h("div", { class: "kpis", id: "md-trade" }),
        h("div", { class: "warn", id: "md-trade-time-note", role: "status", hidden:true }),
        h("div", { class: "warn", id: "md-error", role: "status", "aria-live": "polite" }),
        h("section", { class: "card" },
          h("div", { class: "table-scroll" }, h("table", { class: "data", id: "md-windows" },
            h("caption", { class: "help", style: "text-align:left;padding:0 0 8px", text: t("md_table_caption") }),
            h("thead", null,
              h("tr", null, headCell("md_window", { rowspan: 2 }), headCell("md_market_volume", { rowspan: 2, class: "right" }),
                h("th", { scope: "colgroup", colspan: 3, style: "text-align:center", text: t("md_our_volume") }), headCell("md_vwap_estimate", { rowspan: 2, class: "right" })),
              h("tr", null, headCell("md_bought", { class: "right" }), headCell("md_sold", { class: "right" }), headCell("md_gross", { class: "right" }))),
            h("tbody", { id: "md-window-rows" }))),
          h("p", { class: "help", text: t("md_method_note") }),
          h("p", { class: "help", text: t("md_partial_note") }),
          h("div", { class: "faint", id: "md-updated", "aria-live": "polite" })),
        h("section", { class: "card", style: "margin-top:var(--gap)" },
          h("div", { class: "card-head" }, h("h2", { class: "card-title", text: t("md_live_quotes") }), h("div", { class: "card-sub", text: t("md_note_live") })),
          h("div", { class: "table-scroll" }, h("table", { class: "data", id: "md-table" },
            h("thead", null, h("tr", null, h("th", { text: t("symbol") }), h("th", { class: "right", text: t("md_prev_close") }), h("th", { class: "right", text: t("md_last") }), h("th", { class: "right", text: t("md_change") }), h("th", { class: "right", text: t("md_change_pct") }), h("th", { class: "right", text: t("md_volume") }), h("th", { text: t("md_sparkline") }), h("th", { class: "right", text: t("md_asof") }))),
            h("tbody", null, symbols.map((sym) => h("tr", { dataset: { symbol: sym } }, h("td", null, h("b", { text: sym })), h("td", { class: "right num", dataset: { col: "prev" } }, "—"), h("td", { class: "right num", dataset: { col: "last" } }, "—"), h("td", { class: "right num", dataset: { col: "chg" } }, "—"), h("td", { class: "right num", dataset: { col: "chgpct" } }, "—"), h("td", { class: "right num", dataset: { col: "vol" } }, "—"), h("td", { dataset: { col: "spark" } }), h("td", { class: "right num faint", dataset: { col: "asof" } }, "—"))))))));
      this.select = select;
      this.requestId = 0;
      this.pending = null;
      this.renderTrade(null);
      if (symbols.length) { this.showEmpty(t("md_loading")); this.poll(); } else this.showEmpty(t("md_no_symbols"));
      this.timer = setInterval(() => this.poll(), 5000);
      this.update(new Set(["quotes"]));
    },
    destroy() { clearInterval(this.timer); this.requestId++; },
    zoneTime(epoch, zone, withDate) {
      if (epoch === null || epoch === undefined) return "—";
      return new Date(epoch * 1000).toLocaleString(state.lang === "zh" ? "zh-CN" : "en-GB", Object.assign({ timeZone: zone, hourCycle: "h23", hour: "2-digit", minute: "2-digit" }, withDate ? { month: "short", day: "2-digit", second: "2-digit" } : {}));
    },
    renderTrade(data) {
      const trade = data ? data.last_trade : null;
      const activity = data ? data.last_volume_activity : null;
      const zone = data ? data.timezone : "America/New_York";
      const observed = activity && activity.observed_at !== null && activity.observed_at !== undefined;
      const interval = observed ? this.zoneTime(activity.interval_start, zone, true) + " – " + this.zoneTime(activity.observed_at, zone, true) : "";
      $("#md-trade").replaceChildren(
        kpi(t("md_latest_volume_activity"), observed ? this.zoneTime(activity.observed_at, zone, true) : "—",
          observed ? fmt.qty(activity.quantity) + " " + t("md_activity_shares") + " · " + interval : t("md_no_observed_activity")),
        kpi(t("md_last_trade_time"), trade ? this.zoneTime(trade.time, zone, true) : "—", ""),
        kpi(t("md_last_trade_quantity"), trade ? fmt.qty(trade.quantity) : "—", ""),
        kpi(t("md_last_trade_price"), trade && trade.price !== null ? fmt.price(trade.price) : "—", ""));
      const note = $("#md-trade-time-note");
      note.hidden = !(data && data.volume_newer_than_reported_trade);
      note.textContent = note.hidden ? "" : t("md_trade_time_lags_volume");
    },
    showEmpty(message) {
      const rows = $("#md-window-rows");
      if (!rows) return;
      rows.replaceChildren(h("tr", null, h("td", { colspan: 6, class: "empty", text: message })));
      this.renderTrade(null);
      $("#md-updated").textContent = "";
    },
    async poll() {
      const symbol = this.select && this.select.value;
      if (!symbol || this.pending === symbol || !$("#md-windows")) return;
      const id = ++this.requestId;
      this.pending = symbol;
      const error = $("#md-error");
      try {
        const data = await api("/api/market-data?symbol=" + encodeURIComponent(symbol));
        if (id !== this.requestId || symbol !== this.select.value) return;
        if (data.symbol !== symbol || !Array.isArray(data.windows)) throw new Error(t("md_unavailable"));
        const cell = (value, complete, digits) => {
          const n = Number(value);
          const text = value === null || value === undefined || !Number.isFinite(n) ? "—" : n.toLocaleString(undefined, { maximumFractionDigits: digits });
          return h("td", { class: "right num", title: complete ? null : t("md_partial"), text: complete ? text : text + " *" });
        };
        $("#md-window-rows").replaceChildren(...data.windows.map((w) => h("tr", { class: w.current ? "current" : null },
          h("th", { scope: "row", style: "text-align:left;font-weight:600", text: this.zoneTime(w.start, data.timezone, true).replace(/:\d{2}$/, "") + " – " + this.zoneTime(w.end, data.timezone, false) + (w.current ? " · " + t("md_current") : "") }),
          cell(w.market_volume, w.market_complete, 0), cell(w.bought, w.our_complete, 4), cell(w.sold, w.our_complete, 4), cell(w.gross, w.our_complete, 4), cell(w.vwap_estimate, w.market_complete, 4))));
        this.renderTrade(data);
        error.textContent = data.quote_state !== "live" || data.order_feed !== "live" ? t("md_feed_warning") : "";
        $("#md-updated").textContent = t("md_updated") + " " + this.zoneTime(data.generated_at, data.timezone, true);
      } catch (err) {
        if (id !== this.requestId || symbol !== this.select.value) return;
        if (err instanceof AuthError) { showLogin(err.message); return; }
        this.showEmpty(t("md_unavailable"));
        error.textContent = err.status === 403 ? t("md_forbidden") : err.message || t("md_unavailable");
        if (err.status === 403) clearInterval(this.timer);
      } finally {
        if (id === this.requestId) this.pending = null;
      }
    },
    update(topics) {
      if (!topics.has("quotes")) return;
      const table = $("#md-table");
      if (!table) return;
      for (const tr of $$("tbody tr", table)) {
        const symbol = tr.dataset.symbol;
        const row = state.rows[symbol];
        const prev = state.prevRows[symbol];
        const cell = (col) => tr.querySelector("[data-col='" + col + "']");
        if (!row) continue;
        cell("prev").textContent = fmt.price(row.prev_close);
        if (row.error) {
          cell("last").textContent = row.error;
          ["chg", "chgpct", "vol"].forEach((c) => (cell(c).textContent = "—"));
          cell("asof").textContent = fmt.epoch(row.asof_epoch);
          continue;
        }
        const lastCell = cell("last");
        lastCell.textContent = fmt.price(row.last);
        if (prev && typeof prev.last === "number" && typeof row.last === "number" && prev.last !== row.last) { lastCell.classList.remove("flash-up", "flash-down"); void lastCell.offsetWidth; lastCell.classList.add(row.last > prev.last ? "flash-up" : "flash-down"); }
        const chg = cell("chg"); chg.textContent = fmt.signed(row.change, 4); chg.className = "right num " + (row.change > 0 ? "up" : row.change < 0 ? "down" : "");
        const pct = cell("chgpct"); pct.textContent = fmt.pct(row.change_pct); pct.className = "right num " + (row.change_pct > 0 ? "up" : row.change_pct < 0 ? "down" : "");
        cell("vol").textContent = fmt.int(row.volume);
        cell("asof").textContent = fmt.epoch(row.asof_epoch);
        cell("spark").replaceChildren(sparkline(state.midHistory.get(symbol) || []));
      }
    },
  };

  function sparkline(values) {
    const w = 110, hgt = 26;
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 " + w + " " + hgt);
    svg.setAttribute("class", "sparkline");
    if (values.length < 2) return svg;
    const min = Math.min(...values), max = Math.max(...values);
    const span = max - min || 1;
    const points = values.map((v, i) => ((i / (values.length - 1)) * (w - 2) + 1).toFixed(1) + "," + (hgt - 2 - ((v - min) / span) * (hgt - 4)).toFixed(1)).join(" ");
    const path = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
    path.setAttribute("points", points);
    path.setAttribute("fill", "none");
    path.setAttribute("stroke", values[values.length - 1] >= values[0] ? "var(--green)" : "var(--red)");
    path.setAttribute("stroke-width", "1.5");
    svg.append(path);
    return svg;
  }

  // ---- Market insights --------------------------------------------------------------------
  pages["market-insights"] = {
    render(root) {
      const maxLevels = state.boot.market_insights_max_levels || 20;
      const depthChoices = [5, 10, 20].filter((d) => d <= maxLevels);
      if (!depthChoices.includes(maxLevels)) depthChoices.push(maxLevels);
      const symbols = state.boot.symbols.slice().sort();
      const savedSymbol = storage.get("ktrader.insights.symbol");
      const symbolSel = h("select", { id: "mi-symbol" }, symbols.map((s) => h("option", { value: s, text: s })));
      if (savedSymbol && symbols.includes(savedSymbol)) symbolSel.value = savedSymbol;
      const depthSel = h("select", { id: "mi-depth" }, depthChoices.map((d) => h("option", { value: d, text: String(d), selected: d === Math.max(...depthChoices) })));
      const savedDepth = storage.get("ktrader.insights.depth");
      if (savedDepth && depthChoices.includes(Number(savedDepth))) depthSel.value = savedDepth;
      const viewMode = radioGroup("mi-view", [{ value: "ladder", label: t("depth_ladder") }, { value: "tables", label: t("depth_tables") }], storage.get("ktrader.insights.view") || "ladder");
      const reopen = () => { storage.set("ktrader.insights.symbol", symbolSel.value); storage.set("ktrader.insights.depth", depthSel.value); openBookStream(symbolSel.value, depthSel.value); this.update(new Set(["book"])); };
      symbolSel.addEventListener("change", reopen);
      depthSel.addEventListener("change", reopen);
      viewMode.el.addEventListener("change", () => { storage.set("ktrader.insights.view", viewMode.value()); this.update(new Set(["book"])); });
      this.viewMode = viewMode;
      root.append(h("div", { class: "page-head" }, h("h1", { class: "page-title", text: t("mi_title") }), h("div", { class: "toolbar" },
        h("div", { class: "field" }, h("label", { for: "mi-symbol", text: t("mi_symbol") }), symbolSel), h("div", { class: "field" }, h("label", { for: "mi-depth", text: t("mi_depth") }), depthSel), h("div", { class: "field" }, h("label", { text: " " }), viewMode.el))),
        h("div", { class: "book-stats", id: "mi-stats" }),
        h("div", { id: "mi-body" }));
      reopen();
    },
    update(topics) {
      if (!topics.has("book")) return;
      const body = $("#mi-body"), stats = $("#mi-stats");
      if (!body) return;
      const book = state.book;
      const symbolSel = $("#mi-symbol");
      if (book && Array.isArray(book.symbols) && symbolSel) {
        const current = Array.from(symbolSel.options, (o) => o.value);
        if (JSON.stringify(current) !== JSON.stringify(book.symbols)) {
          const selected = symbolSel.value;
          symbolSel.replaceChildren(...book.symbols.map((s) => h("option", { value: s, text: s })));
          if (book.symbols.includes(selected)) symbolSel.value = selected;
        }
      }
      if (!book) { body.replaceChildren(h("div", { class: "empty", text: t("loading") })); stats.replaceChildren(); return; }
      const bids = book.bids || [], asks = book.asks || [];
      const bestBid = bids.length ? bids[0].price : null, bestAsk = asks.length ? asks[0].price : null;
      const spread = bestBid !== null && bestAsk !== null ? bestAsk - bestBid : null;
      const mid = bestBid !== null && bestAsk !== null ? (bestBid + bestAsk) / 2 : null;
      const sum = (levels) => levels.reduce((s, l) => s + (Number.isFinite(Number(l.quantity)) ? Number(l.quantity) : 0), 0);
      stats.replaceChildren(
        kpi(t("mi_bid"), fmt.price(bestBid), fmt.int(bids.length ? bids[0].quantity : null)),
        kpi(t("mi_ask"), fmt.price(bestAsk), fmt.int(asks.length ? asks[0].quantity : null)),
        kpi(t("book_spread"), spread === null ? "—" : fmt.price(spread), mid !== null && spread !== null ? (spread / mid * 10000).toFixed(1) + " bp" : ""),
        kpi(t("book_mid"), fmt.price(mid), ""),
        kpi(t("book_imbalance"), fmt.int(sum(bids)) + " / " + fmt.int(sum(asks)), ""),
        kpi(t("book_updated"), fmt.epoch(book.asof_epoch), book.error ? book.error : ""));
      if (book.error || (!bids.length && !asks.length)) { body.replaceChildren(h("div", { class: "empty", text: t("mi_no_book") })); return; }
      const maxCum = Math.max(sum(bids), sum(asks)) || 1;
      const side = (levels, isBid) => {
        let cum = 0;
        const rows = levels.map((l) => {
          const q = Number(l.quantity);
          if (Number.isFinite(q)) cum += q;
          const width = Math.min(100, (cum / maxCum) * 100).toFixed(1) + "%";
          const cells = [h("td", { class: "right price " + (isBid ? "bid" : "ask") }, h("span", { text: fmt.price(l.price) })), h("td", { class: "right" }, h("span", { text: fmt.num(l.quantity, 2) })), h("td", { class: "right" }, h("span", { text: fmt.int(cum) })), h("td", { class: "right" }, h("span", { text: fmt.int(l.order_count) }))];
          const tr = h("tr", { class: isBid ? "bid" : "ask" }, isBid ? cells : cells);
          const bar = h("span", { class: "bar", style: "width:" + width });
          cells[1].prepend(bar);
          return tr;
        });
        return h("section", { class: "card" }, h("div", { class: "card-head" }, h("h2", { class: "card-title", text: isBid ? t("mi_bid") : t("mi_ask") }), h("div", { class: "card-sub", text: String(levels.length) })),
          h("table", { class: "data" }, h("thead", null, h("tr", null, h("th", { class: "right", text: t("mi_price") }), h("th", { class: "right", text: t("mi_qty") }), h("th", { class: "right", text: t("mi_cum_qty") }), h("th", { class: "right", text: t("mi_orders") }))), h("tbody", null, rows)));
      };
      const ladder = h("div", { class: "ladder" + (this.viewMode.value() === "tables" ? " plain" : "") }, side(bids, true), side(asks, false));
      if (this.viewMode.value() === "tables") $$(".bar", ladder).forEach((b) => b.remove());
      body.replaceChildren(ladder);
    },
  };

  // ---- Admin control (lmh only) ------------------------------------------------------------------
  // One read-only GET_BROKER_ORDERS request for a New York trading day and a set of accounts.
  pages["admin-control"] = {
    render(root) {
      const metas = state.boot.accounts;  // sorted by numeric id
      const today = state.boot.broker_orders_today;
      const date = h("input", { id: "broker-orders-date", type: "date", required: true, max: today, value: today });
      const all = h("input", { class: "choice-input", type: "checkbox", id: "broker-orders-all", name: "account_ids", value: "__ALL__", checked: true });
      const accounts = metas.map((m, i) => h("input", { class: "choice-input", type: "checkbox", id: "broker-orders-account-" + i, name: "account_ids", value: m.id, dataset: { brokerOrdersAccount: "" } }));
      const submit = h("button", { class: "btn", type: "submit" }, t("broker_orders_submit"));
      const message = h("div", { class: "help", role: "status", "aria-live": "polite" });
      const sync = () => { submit.disabled = !all.checked && !accounts.some((i) => i.checked); };
      all.addEventListener("change", () => { if (all.checked) accounts.forEach((i) => (i.checked = false)); sync(); });
      accounts.forEach((i) => i.addEventListener("change", () => { if (i.checked) all.checked = false; sync(); }));
      const form = h("form", { id: "broker-orders-form", onsubmit: async (e) => {
        e.preventDefault();
        if (!form.checkValidity()) { form.reportValidity(); return; }
        const ids = all.checked ? ["__ALL__"] : accounts.filter((i) => i.checked).map((i) => i.value);
        if (!ids.length) { message.className = "warn"; message.textContent = t("quick_pick_account"); return; }
        submit.disabled = true;
        message.className = "help";
        message.textContent = t("submitting");
        try {
          const result = await api("/api/broker-orders", { json: { trading_date: date.value, account_ids: ids } });
          message.className = "ok-msg";
          message.textContent = result.message;
          toast("ok", result.message);
        } catch (err) {
          message.className = "warn";
          message.textContent = err.message || t("broker_orders_publish_failed");
          if (err instanceof AuthError) showLogin(err.message);
        } finally { sync(); }
      } },
        h("div", { class: "field" }, h("label", { for: "broker-orders-date", text: t("broker_orders_date") }), date),
        h("div", { class: "field" }, h("label", { text: t("cancel_accounts") }),
          h("div", { class: "choice-grid", role: "group" }, all, h("label", { class: "choice-btn", for: "broker-orders-all", text: t("all_accounts") }),
            metas.map((m, i) => [accounts[i], h("label", { class: "choice-btn", for: accounts[i].id, title: [m.id, m.broker].join(" | "), text: "#" + m.num_id + " " + m.id })]))),
        h("div", { class: "help", text: t("broker_orders_help") }),
        message,
        submit);
      root.append(h("div", { class: "page-head" }, h("h1", { class: "page-title", text: t("admin_control") })),
        h("div", { class: "grid-2" }, h("div", { class: "col" }, card(t("broker_orders_title"), form))));
      // A submitted page restored from the back/forward cache must be usable again.
      this.onPageShow = () => sync();
      window.addEventListener("pageshow", this.onPageShow);
      sync();
    },
    destroy() { if (this.onPageShow) window.removeEventListener("pageshow", this.onPageShow); },
  };

  // ---- Trading status ---------------------------------------------------------------------
  pages["trading-status"] = {
    render(root) {
      const msg = h("div", { class: "warn", id: "ts-msg" });
      const offAll = h("button", { class: "btn red", type: "button", onclick: async (e) => { if (!(await confirmDialog(t("confirm_turn_off_all")))) return; submitForm(e, "/api/trading-status", () => ({ disable_all: true, trading_enabled: false }), msg); } }, t("turn_off_all"));
      root.append(h("div", { class: "page-head" }, h("h1", { class: "page-title", text: t("trading_status") }), h("div", { class: "inline-actions" }, offAll)),
        h("section", { class: "card", style: "max-width:920px" }, h("div", { class: "card-head" }, h("h2", { class: "card-title", text: t("current_status") })), msg, h("div", { class: "status-list", id: "trading-status-list" })));
      this.update(new Set(["accounts"]));
    },
    update(topics) {
      if (!topics.has("accounts")) return;
      const list = $("#trading-status-list");
      if (!list) return;
      const accounts = (state.accounts && state.accounts.accounts) || [];
      const msg = $("#ts-msg");
      list.replaceChildren(...state.boot.accounts.map((m) => {
        const a = accounts.find((x) => x.account_id === m.id);
        const known = !!a, enabled = !!(a && a.trading_enabled);
        const metaLine = [m.broker, m.trading_medium, m.machine_alias, m.ip_address, m.broker_id].filter(Boolean).join(" · ");
        return h("div", { class: "status-item trading-status-row", dataset: { accountId: m.id } },
          h("div", null, h("div", { class: "status-title", text: "#" + m.num_id + " " + m.id }), h("div", { class: "status-meta", text: metaLine }), h("div", { class: "status-row" }, t("trading") + ":", h("span", { class: "pill " + (known ? (enabled ? "on" : "off") : "unknown"), text: known ? t(enabled ? "trading_on" : "trading_off") : t("trading_unknown") }))),
          h("div", { class: "inline-actions" },
            h("button", { class: "btn sm green", type: "button", disabled: known && enabled, onclick: (e) => submitForm(e, "/api/trading-status", () => ({ account_id: m.id, trading_enabled: true }), msg) }, t("turn_on")),
            h("button", { class: "btn sm red", type: "button", disabled: known && !enabled, onclick: (e) => submitForm(e, "/api/trading-status", () => ({ account_id: m.id, trading_enabled: false }), msg) }, t("turn_off"))));
      }));
    },
  };

  // ---- Health dialog -------------------------------------------------------------------------
  async function openHealth() {
    let health;
    try { health = await api("/api/health"); } catch (e) { toast("err", e.message); return; }
    const feeds = health.feeds || {};
    const feedRows = ["accounts", "market", "orders", "volume", "buying_orders", "buying_volume", "own_orders"].filter((k) => feeds[k] !== null && feeds[k] !== undefined || ["accounts", "market"].includes(k)).map((k) => {
      const f = feeds[k];
      const rejected = f ? (f.rejected !== undefined ? f.rejected : f.invalid_messages) : null;
      return h("tr", null, h("td", { text: t("feed_" + k) }), h("td", null, f ? h("span", { class: "pill " + (feedClass(f) === "live" ? "on" : feedClass(f) === "down" ? "off" : feedClass(f) === "stale" ? "stale" : "unknown"), text: t("feed_" + (feedClass(f) === "stale" ? "stale" : f.state || "unavailable")) }) : h("span", { class: "pill unknown", text: t("feed_unavailable") })), h("td", { class: "num", text: f && f.last_message_at ? fmt.age(f.last_message_at) : "—" }), h("td", { class: "right num", text: f && f.messages !== undefined ? fmt.int(f.messages) : "—" }), h("td", { class: "right num", text: rejected !== null && rejected !== undefined ? fmt.int(rejected) : "—" }), h("td", { class: "faint", text: f && f.last_error ? f.last_error : (f && f.detail ? f.detail : (f && f.topic ? f.topic : "")) }));
    });
    const cmd = health.commands || {};
    const body = h("div", null,
      h("dl", { class: "kv" }, h("dt", { text: t("health_version") }), h("dd", { text: health.version }), h("dt", { text: t("health_uptime") }), h("dd", { text: Math.round(health.uptime_seconds / 60) + " min (" + fmt.time(health.started_at) + ")" }), h("dt", { text: t("health_sse_clients") }), h("dd", { text: String(health.live_connections) }), h("dt", { text: t("health_kafka_commands") }), h("dd", { text: fmt.int(cmd.published) + " / " + fmt.int(cmd.delivered) + " " + t("published") + (cmd.failed ? " · " + fmt.int(cmd.failed) + " failed" : "") }), h("dt", { text: t("operations") }), h("dd", { text: health.operations_log || "—" })),
      h("div", { class: "section-heading", text: t("feed_accounts") + " / " + t("feed_market") }),
      h("table", { class: "data" }, h("thead", null, h("tr", null, h("th"), h("th", { text: t("status") }), h("th", { text: t("last_update") }), h("th", { class: "right", text: "msgs" }), h("th", { class: "right", text: "rejected" }), h("th"))), h("tbody", null, feedRows)));
    openModal(t("health_title"), body, { actions: [h("button", { class: "btn gray", type: "button", onclick: closeModal }, t("modal_close"))] });
  }

  // ---- Login -----------------------------------------------------------------------------
  function showLogin(message) {
    if (state.stream) { state.stream.close(); state.stream = null; }
    closeBookStream();
    $("#topbar").hidden = true;
    const content = $("#content");
    content.replaceChildren();
    const username = h("input", { id: "username", name: "username", type: "text", autocomplete: "username", required: true });
    const password = h("input", { id: "password", name: "password", type: "password", autocomplete: "current-password", required: true });
    const error = h("div", { class: "warn", text: message || "" });
    const langBtn = h("button", { class: "lang-btn", type: "button", onclick: () => setLang(state.lang === "zh" ? "en" : "zh").then(() => showLogin("")) }, state.lang === "zh" ? "EN" : "中文");
    const form = h("form", { onsubmit: async (e) => {
      e.preventDefault();
      error.textContent = "";
      try {
        await api("/login", { form: { username: username.value.trim(), password: password.value, next: location.pathname } });
        await boot();
      } catch (err) { error.textContent = err.message || t("login_failed"); }
    } },
      h("div", { class: "field" }, h("label", { for: "username", text: t("username") }), username),
      h("div", { class: "field" }, h("label", { for: "password", text: t("password") }), password),
      error,
      h("button", { class: "btn", type: "submit" }, t("login")));
    content.append(h("div", { class: "login-wrap" }, h("div", { class: "card login-card" }, h("div", { class: "login-brand" }, h("span", { class: "brand-mark" }), h("h1", { text: "KTrader" }), h("span", { style: "margin-left:auto" }, langBtn)), h("div", { class: "help", style: "margin-bottom:12px" }, t("login_welcome") + " · " + t("login_hint")), form)));
    username.focus();
  }

  // ---- Bootstrapping -----------------------------------------------------------------------
  async function loadDictionary(lang) {
    state.lang = lang;
    state.T = await fetch("/api/i18n?lang=" + encodeURIComponent(lang), { cache: "no-store" }).then((r) => r.json());
  }
  async function setLang(lang) {
    document.cookie = "lang=" + lang + "; path=/; max-age=15552000; samesite=lax";
    await loadDictionary(lang);
    if (state.boot) { renderNav(); renderPage(); }
  }

  async function boot() {
    // Have the embedded CJK fallback face ready before the first layout, so text is never shaped
    // with a glyph-less fallback on machines without a Chinese system font.
    try { if (document.fonts && document.fonts.load) await document.fonts.load('400 16px "KTrader CJK"'); } catch (e) { /* ignore */ }
    // A ?lang= link wins over the cookie, as in the original pages; it is remembered so the
    // server's own messages use the same language.
    const queryLang = new URLSearchParams(location.search).get("lang");
    const cookieLang = getCookie("lang");
    let lang = cookieLang === "zh" || cookieLang === "en" ? cookieLang : (navigator.language || "").toLowerCase().startsWith("zh") ? "zh" : "en";
    if ((queryLang === "zh" || queryLang === "en") && queryLang !== cookieLang) {
      lang = queryLang;
      document.cookie = "lang=" + lang + "; path=/; max-age=15552000; samesite=lax";
    }
    if (!state.T || !Object.keys(state.T).length || state.lang !== lang) await loadDictionary(lang);
    let bootstrap;
    try {
      bootstrap = await api("/api/bootstrap");
    } catch (e) {
      showLogin(e instanceof AuthError ? "" : e.message);
      return;
    }
    state.boot = bootstrap;
    $("#topbar").hidden = false;
    renderNav();
    await refreshAll();
    if (location.pathname === "/login" || location.pathname === "/") history.replaceState({}, "", "/account-details");
    showLegacyFlash();
    setRoute();
    openStream();
  }

  // Legacy form endpoints (/submit-order, /submit-algo, ...) redirect back with ?ok= / ?err= messages.
  function showLegacyFlash() {
    const params = new URLSearchParams(location.search);
    const ok = params.get("ok");
    const err = params.get("err");
    if (!ok && !err) return;
    if (ok) toast("ok", ok);
    if (err) toast("err", err, 10000);
    params.delete("ok");
    params.delete("err");
    const rest = params.toString();
    history.replaceState({}, "", location.pathname + (rest ? "?" + rest : "") + location.hash);
  }

  function init() {
    applyTheme(storage.get("ktrader.theme") || (window.matchMedia && window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark"));
    $("#theme-btn").addEventListener("click", () => applyTheme(document.documentElement.dataset.theme === "dark" ? "light" : "dark"));
    $("#lang-btn").addEventListener("click", () => setLang(state.lang === "zh" ? "en" : "zh"));
    $("#health-btn").innerHTML = ICONS.pulse;
    $("#health-btn").addEventListener("click", openHealth);
    $("#logout-link").addEventListener("click", async (e) => { e.preventDefault(); try { await api("/api/logout", { method: "POST" }); } catch (err) { /* ignore */ } state.boot = null; history.replaceState({}, "", "/login"); showLogin(""); });
    boot();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
})();
