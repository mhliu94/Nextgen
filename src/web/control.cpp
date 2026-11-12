#include <malloc.h>
#include "web/control.h"
#include "core/public_payload.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "orders/commands.h"

namespace ng::web {
namespace {
constexpr const char* kLog = "trading-ui.control";
}

// ---------------------------------------------------------------------------
// RecordingCommandSink
// ---------------------------------------------------------------------------

void RecordingCommandSink::publish(const json& command, const std::string& key) {
    std::lock_guard lock(mutex_);
    if (!fail_next_.empty()) {
        std::string error = fail_next_;
        fail_next_.clear();
        throw std::runtime_error(error);
    }
    history_.push_back(kafka::PublishedCommand{wall_now(), key, command, "recorded", ""});
}

std::vector<kafka::PublishedCommand> RecordingCommandSink::history() const {
    std::lock_guard lock(mutex_);
    return history_;
}

json RecordingCommandSink::stats() const {
    std::lock_guard lock(mutex_);
    json out;
    out["published"] = static_cast<long long>(history_.size());
    out["delivered"] = static_cast<long long>(history_.size());
    out["failed"] = 0;
    out["outstanding"] = 0;
    out["topic"] = "(recording)";
    return out;
}

void RecordingCommandSink::clear() {
    std::lock_guard lock(mutex_);
    history_.clear();
}

std::size_t RecordingCommandSink::count() const {
    std::lock_guard lock(mutex_);
    return history_.size();
}

// ---------------------------------------------------------------------------
// Pure views
// ---------------------------------------------------------------------------

bool snapshot_is_stale(const std::optional<std::string>& ts, double now_epoch) {
    if (!ts || ts->empty()) return false;
    auto parsed = parse_iso8601(*ts);
    if (!parsed) return false;
    return now_epoch - parsed->epoch_seconds > 5 * 60;
}

json account_meta_view(const AccountMeta& meta) {
    json out;
    out["id"] = meta.id;
    out["num_id"] = meta.num_id;
    out["broker"] = meta.broker;
    out["trading_medium"] = meta.trading_medium;
    out["broker_id"] = meta.broker_id;
    out["ip_address"] = meta.ip_address ? json(*meta.ip_address) : json(nullptr);
    out["machine_alias"] = meta.machine_alias ? json(*meta.machine_alias) : json(nullptr);
    out["monitor"] = meta.monitor;
    out["label"] = meta.label();
    return out;
}

json portfolio_view(const std::map<std::string, AccountSnapshot>& accounts, const AccountDirectory& metas,
                    const std::map<std::string, StockQuote>& quotes) {
    std::map<std::string, double> cash_totals;
    struct Aggregate {
        double qty = 0, priced_value = 0, priced_qty = 0;
        bool all_priced = true;
        std::set<int> directions;
        int account_count = 0;
    };
    std::map<std::string, Aggregate> positions;
    int included = 0;
    for (const AccountMeta& meta : metas.ordered()) {
        auto it = accounts.find(meta.id);
        if (it == accounts.end()) continue;
        const AccountSnapshot& account = it->second;
        ++included;
        std::map<std::string, double> balances;
        for (auto& [raw_currency, amount] : account.cash_by_currency) {
            std::string currency = to_upper(trim(raw_currency));
            if (currency.empty()) continue;
            balances[currency] = std::isfinite(amount) ? amount : 0.0;
        }
        if (!balances.count("USD")) balances["USD"] = std::isfinite(account.cash) ? account.cash : 0.0;
        for (auto& [currency, amount] : balances) cash_totals[currency] += amount;
        std::set<std::string> seen;
        for (const Position& position : account.positions) {
            std::string symbol = to_upper(trim(position.symbol));
            if (symbol.empty()) continue;
            double qty = std::isfinite(position.qty) ? position.qty : 0.0;
            if (qty == 0.0) continue;
            Aggregate& agg = positions[symbol];
            agg.qty += qty;
            agg.directions.insert(qty > 0 ? 1 : -1);
            if (!position.avg_price || !std::isfinite(*position.avg_price)) agg.all_priced = false;
            else {
                agg.priced_qty += std::fabs(qty);
                agg.priced_value += std::fabs(qty) * *position.avg_price;
            }
            if (!seen.count(symbol)) {
                agg.account_count += 1;
                seen.insert(symbol);
            }
        }
    }
    json out;
    out["cash"] = json::object();
    for (auto& [currency, amount] : cash_totals) out["cash"][currency] = amount;
    out["positions"] = json::array();
    double total_market_value = 0.0;
    bool any_unpriced = false;
    for (auto& [symbol, agg] : positions) {
        json row;
        row["symbol"] = symbol;
        row["qty"] = agg.qty;
        if (agg.all_priced && agg.directions.size() == 1 && agg.priced_qty > 0) row["avg_price"] = agg.priced_value / agg.priced_qty;
        else row["avg_price"] = nullptr;
        auto q = quotes.find(symbol);
        if (q != quotes.end() && q->second.usable()) {
            double last = *q->second.last;
            double market_value = agg.qty * last;
            row["last"] = last;
            row["market_value"] = std::isfinite(market_value) ? json(market_value) : json(nullptr);
            if (std::isfinite(market_value)) total_market_value += market_value;
            else any_unpriced = true;
        } else {
            row["last"] = nullptr;
            row["market_value"] = nullptr;
            any_unpriced = true;
        }
        row["account_count"] = agg.account_count;
        out["positions"].push_back(row);
    }
    out["included_accounts"] = included;
    out["total_accounts"] = static_cast<long long>(metas.size());
    out["total_market_value"] = total_market_value;
    out["market_value_complete"] = !any_unpriced;
    return out;
}

// ---------------------------------------------------------------------------
// ControlCenter
// ---------------------------------------------------------------------------

ControlCenter::ControlCenter(AppConfig config, AccountDirectory accounts, std::vector<std::string> symbols)
    : config_(std::move(config)),
      accounts_(std::move(accounts)),
      symbols_(std::move(symbols)),
      started_at_(wall_now()) {
    account_store_.add_listener([this] { notify_hub(kTopicAccounts); });
    market_store_.add_quote_listener([this](const std::string&) { notify_hub(kTopicQuotes | kTopicAccounts); });
    market_store_.add_book_listener([this](const std::string& symbol) { notify_hub(kTopicBook, symbol); });
}

ControlCenter::~ControlCenter() { shutdown(); }

void ControlCenter::set_command_sink(std::unique_ptr<CommandSink> sink) { sink_ = std::move(sink); }

void ControlCenter::set_operations_log(std::unique_ptr<OperationsLog> log) { operations_log_ = std::move(log); }

void ControlCenter::create_reporting(const std::string& db_path) {
    reporting_ = std::make_unique<reporting::ActivityReporting>(db_path);
    market_activity_ = std::make_unique<reporting::MarketActivity>(db_path);
    // Our traded volume covers all visible configured accounts; its fills are attributed in New York time.
    market_order_store_ = std::make_unique<order_ui::OrderStore>(
        accounts_, reporting::MarketActivity::kMarketZone, nullptr,
        [this](const orders::OrderEvent& event) { market_activity_->record_order(event); });
}

std::vector<smart::SmartSellingRuntime*> ControlCenter::smart_managers() const {
    std::vector<smart::SmartSellingRuntime*> managers;
    if (smart_) managers.push_back(smart_.get());
    if (buying_) managers.push_back(buying_.get());
    return managers;
}

void ControlCenter::create_fast_trading_manager() {
    fast::RuntimeSettings settings;
    for (auto& [level, cfg] : config_.fast_trading.aggression_levels) settings.aggression_levels[level] = {cfg.book_levels, cfg.cycle_seconds};
    settings.minimum_cycle_seconds_by_medium = config_.fast_trading.minimum_cycle_seconds_by_medium;
    fast_ = std::make_unique<fast::StrategyManager>(
        market_store_, [this]() -> const AccountDirectory& { return accounts_; },
        [this](const json& command, const std::string& key) { publish_fast_trading_command(command, key); },
        [this] { return served_snapshots(); }, operations_log_.get(), settings, nullptr, reporting_.get(),
        session_coordinator_);
}

void ControlCenter::create_smart_selling_runtime(std::shared_ptr<smart::SmartSellingEngine> engine) {
    smart_ = std::make_unique<smart::SmartSellingRuntime>(
        config_.smart_selling, [this]() -> const AccountDirectory& { return accounts_; }, [this] { return served_snapshots(); },
        [this](const json& command, const std::string& key) { publish_trading_command(command, key); },
        session_coordinator_,
        std::move(engine), nullptr, nullptr, reporting_.get());
}

void ControlCenter::create_smart_buying_runtime(std::shared_ptr<smart::SmartBuyingEngine> engine) {
    buying_ = std::make_unique<smart::SmartBuyingRuntime>(
        config_.smart_buying, market_store_, [this]() -> const AccountDirectory& { return accounts_; },
        [this] { return served_snapshots(); },
        [this](const json& command, const std::string& key) { publish_trading_command(command, key); },
        session_coordinator_,
        std::move(engine), nullptr, nullptr, reporting_.get());
}

void ControlCenter::set_feed_health_providers(FeedHealthProviders providers) { feed_health_ = std::move(providers); }

namespace {
unsigned housekeeping_ticks = 0;
}  // namespace

void ControlCenter::start_background() {
    if (background_started_.exchange(true)) return;
    if (smart_) smart_->start();
    if (buying_) buying_->start();
    // Periodic status refresh for the browser: algorithm/smart/feed status changes are not store
    // mutations, so push them on a 1 Hz cadence (clients coalesce).
    status_thread_ = std::thread([this] {
        while (!stop_.load()) {
            if (++housekeeping_ticks % 60 == 0) malloc_trim(0);  // return freed arena pages to the OS once a minute
            notify_hub(kTopicSmart | kTopicAlgo | kTopicFeeds | kTopicCommands);
            for (int i = 0; i < 10 && !stop_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void ControlCenter::shutdown() {
    if (stop_.exchange(true)) return;
    hub_.shutdown();
    if (status_thread_.joinable()) status_thread_.join();
    // Also used after partial startup. One failing service must not strand the others.
    auto guarded = [](const char* what, const std::function<void()>& call) {
        try {
            call();
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "Failed shutting down {}: {}", what, e.what());
        }
    };
    if (fast_) guarded("fast trading", [this] { fast_->stop_all(); });
    if (smart_) guarded("Smart selling", [this] { smart_->close(); });
    if (buying_) guarded("Smart buying", [this] { buying_->close(); });
    if (reporting_) guarded("activity reporting", [this] { reporting_->close(); });
    if (market_activity_) guarded("market activity", [this] { market_activity_->close(); });
}

std::map<std::string, AccountSnapshot> ControlCenter::served_snapshots() const {
    std::map<std::string, AccountSnapshot> out;
    for (auto& [id, snap] : account_store_.get_all()) {
        if (accounts_.contains(id)) out[id] = snap;
    }
    return out;
}

std::optional<double> ControlCenter::market_last(const std::string& symbol) const {
    auto rows = market_store_.get_for_symbols({symbol});
    auto it = rows.find(symbol);
    if (it == rows.end() || it->second.error) return std::nullopt;
    return it->second.last;
}

bool ControlCenter::owned_by_algorithm(const std::string& account_id) const {
    if (fast_ && fast_->owns_account(account_id)) return true;
    for (auto* manager : smart_managers()) {
        if (manager->owns_account(account_id)) return true;
    }
    return false;
}

void ControlCenter::publish_trading_command(const json& command, const std::string& key) {
    if (!sink_) throw std::runtime_error("Kafka producer is unavailable");
    sink_->publish(command, key);
    if (operations_log_ == nullptr) return;
    notify_hub(kTopicCommands);
}

void ControlCenter::publish_fast_trading_command(const json& command, const std::string& key) {
    std::lock_guard lock(algo_lock_);
    std::string account = json_to_string(json_find(command, "account_id")).value_or("");
    std::string symbol = json_to_string(json_find(command, "symbol")).value_or("");
    if (!fast_ || !fast_->has_running_strategy(account, symbol)) return;
    for (auto* manager : smart_managers()) {
        // Shared accounts are permitted only on distinct instruments.
        if (manager->owns(account, symbol)) throw std::invalid_argument(
            "Cannot publish algorithm order: account " + account + " already has a Smart algorithm session on symbol " +
            symbol + "; all strategies on an account must use distinct symbols");
        manager->track_external_order(command);
    }
    publish_trading_command(command, key);
}

void ControlCenter::publish_control_command(const json& command, const std::string& key) {
    std::string type = json_to_string(json_find(command, "type")).value_or("");
    bool places_order = type == "LIMIT_ORDER" || type == "LIMIT_ORDER_FOK" || type == "MARKET_ORDER" || type == "DELAYED_MARKET_ORDER";
    std::lock_guard lock(algo_lock_);
    std::string account_id = json_to_string(json_find(command, "account_id")).value_or("");
    if (places_order || type == "CURRENCY_CONVERSION" || starts_with(type, "CANCEL_")) {
        if (owned_by_algorithm(account_id)) {
            throw std::invalid_argument(
                "An algorithm session owns this account; stop it and await outstanding order confirmations first");
        }
    }
    if (places_order) {
        for (auto* manager : smart_managers()) manager->track_external_order(command);
    }
    publish_trading_command(command, key);
}

std::string ControlCenter::start_fast_strategy(const json& command) {
    std::lock_guard lock(algo_lock_);
    if (!fast_) throw std::runtime_error("The algorithm trading manager is unavailable");
    std::string key = fast_->start_strategy(command);
    notify_hub(kTopicAlgo);
    return key;
}

json ControlCenter::start_smart_parent(const json& settings) {
    std::lock_guard lock(algo_lock_);
    if (!smart_) throw std::runtime_error("Smart selling is unavailable");
    json parent = smart_->start_parent(settings);
    notify_hub(kTopicSmart | kTopicAlgo);
    return parent;
}

json ControlCenter::stop_smart_parent(const std::string& parent_id) {
    std::lock_guard lock(algo_lock_);
    if (!smart_) throw std::runtime_error("Smart selling is unavailable");
    json result = smart_->stop_parent(parent_id);
    notify_hub(kTopicSmart | kTopicAlgo);
    return result;
}

json ControlCenter::start_smart_buying_parent(const json& settings) {
    std::lock_guard lock(algo_lock_);
    if (!buying_) throw std::runtime_error("Smart buying is unavailable");
    json parent = buying_->start_parent(settings);
    notify_hub(kTopicSmart | kTopicAlgo);
    return parent;
}

json ControlCenter::stop_smart_buying_parent(const std::string& parent_id) {
    std::lock_guard lock(algo_lock_);
    if (!buying_) throw std::runtime_error("Smart buying is unavailable");
    json result = buying_->stop_parent(parent_id);
    notify_hub(kTopicSmart | kTopicAlgo);
    return result;
}

StopAllResult ControlCenter::stop_all_algorithms(const std::optional<std::string>& reason) {
    // Scope comes from the server, never from submitted mode/account fields. Stop local strategies
    // first even if Kafka is unavailable, and keep new starts behind the stop notification.
    std::lock_guard lock(algo_lock_);
    StopAllResult result;
    if (!fast_ && smart_managers().empty()) {
        throw std::runtime_error("The algorithm trading manager is unavailable; stopping could not be confirmed.");
    }
    result.fast_stopped = fast_ ? fast_->stop_all() : 0;
    result.smart_stopped = smart_ ? smart_->stop_all() : 0;
    result.buying_stopped = buying_ ? buying_->stop_all() : 0;
    result.notification = orders::build_algo_stop_command(reason);
    try {
        if (!sink_) throw std::runtime_error("Kafka producer is unavailable");
        publish_control_command(result.notification, "ALGO_STOP");
        result.notification_published = true;
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "All local algorithm strategies stopped, but stop notification failed: {}", e.what());
        result.notification_error = e.what();
    }
    notify_hub(kTopicSmart | kTopicAlgo);
    return result;
}

bool ControlCenter::any_algorithm_active() const {
    if (fast_ && fast_->has_active_session()) return true;
    for (auto* manager : smart_managers()) {
        if (manager->has_active_session()) return true;
    }
    return false;
}

namespace {
std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2)); }
std::uint64_t time_bucket(double seconds) { return static_cast<std::uint64_t>(mono_now() / seconds); }
}  // namespace

void ControlCenter::notify_hub(unsigned topics, const std::string& symbol) {
    // Market data and account snapshots have their own version counters; only status changes
    // (algorithms, Smart selling, feeds, published commands) invalidate the status documents.
    if (topics & (kTopicSmart | kTopicAlgo | kTopicFeeds | kTopicCommands)) status_generation_.fetch_add(1, std::memory_order_relaxed);
    hub_.notify(topics, symbol);
}

json ControlCenter::browser_payload(const json& payload) const {
    auto out=public_payload(payload);
    if (reporting_) label_sessions(out,reporting_->session_labels());
    return out;
}

std::shared_ptr<const std::string> ControlCenter::cached_text(const std::string& name, std::uint64_t key,
                                                              const std::function<json()>& build) const {
    {
        std::lock_guard lock(text_cache_mutex_);
        auto it = text_cache_.find(name);
        if (it != text_cache_.end() && it->second.first == key) return it->second.second;
    }
    auto text = std::make_shared<const std::string>(json_dump_compact(browser_payload(build())));
    std::lock_guard lock(text_cache_mutex_);
    if (text_cache_.size() > 512) text_cache_.clear();
    text_cache_[name] = {key, text};
    return text;
}

std::shared_ptr<const std::string> ControlCenter::accounts_payload_text(const std::string& lang) const {
    return cached_text("accounts:" + lang, account_store_.version(), [&] { return accounts_payload(lang); });
}

std::shared_ptr<const std::string> ControlCenter::accounts_view_text(const std::string& lang) const {
    std::uint64_t key = mix(mix(account_store_.version(), market_store_.quotes_version()), time_bucket(1.0));
    return cached_text("view:" + lang, key, [&] { return accounts_view(lang); });
}

std::shared_ptr<const std::string> ControlCenter::quotes_payload_text() const {
    std::uint64_t key = mix(market_store_.quotes_version(), time_bucket(1.0));
    return cached_text("quotes", key, [&] { return quotes_payload(std::nullopt); });
}

std::shared_ptr<const std::string> ControlCenter::insights_payload_text(const std::string& symbol, int depth) const {
    std::uint64_t key = mix(mix(market_store_.book_version(symbol), market_store_.books_version()), static_cast<std::uint64_t>(depth));
    return cached_text("insights:" + symbol + ":" + std::to_string(depth), key, [&] { return insights_payload(symbol, depth); });
}

std::shared_ptr<const std::string> ControlCenter::smart_status_text() const {
    return cached_text("smart", mix(status_generation_.load(std::memory_order_relaxed), time_bucket(0.25)), [&] { return smart_status(); });
}

std::shared_ptr<const std::string> ControlCenter::smart_buying_status_text() const {
    return cached_text("buying", mix(status_generation_.load(std::memory_order_relaxed), time_bucket(0.25)),
                       [&] { return smart_buying_status(); });
}

std::shared_ptr<const std::string> ControlCenter::algo_status_text() const {
    return cached_text("algo", mix(status_generation_.load(std::memory_order_relaxed), time_bucket(0.25)), [&] { return algo_status(); });
}

std::shared_ptr<const std::string> ControlCenter::feeds_health_text() const {
    return cached_text("feeds", mix(status_generation_.load(std::memory_order_relaxed), time_bucket(0.5)), [&] { return feeds_health(); });
}

std::shared_ptr<const std::string> ControlCenter::health_text() const {
    return cached_text("health", mix(status_generation_.load(std::memory_order_relaxed), time_bucket(1.0)), [&] { return health(); });
}

std::shared_ptr<const std::string> ControlCenter::commands_history_text() const {
    return cached_text("commands", mix(status_generation_.load(std::memory_order_relaxed), time_bucket(0.25)), [&] { return commands_history(); });
}

json ControlCenter::accounts_payload(const std::string& lang) const {
    auto accounts = served_snapshots();
    std::vector<const AccountSnapshot*> ordered;
    for (auto& [id, snap] : accounts) ordered.push_back(&snap);
    auto account_num = [&](const AccountSnapshot& snap) -> int {
        if (const AccountMeta* meta = accounts_.find(snap.account_id)) return meta->num_id;
        if (snap.account_num_id) return *snap.account_num_id;
        return 999;
    };
    std::sort(ordered.begin(), ordered.end(), [&](const AccountSnapshot* a, const AccountSnapshot* b) {
        int na = account_num(*a), nb = account_num(*b);
        if (na != nb) return na < nb;
        return a->account_id < b->account_id;
    });
    json out;
    out["source"] = "kafka";
    out["lang"] = lang;
    out["accounts"] = json::array();
    for (const AccountSnapshot* snap : ordered) {
        json a = snap->to_json();
        int num = account_num(*snap);
        a["account_num_id"] = num != 999 ? json(num) : json(nullptr);
        out["accounts"].push_back(a);
    }
    return out;
}

json ControlCenter::accounts_view(const std::string& lang) const {
    json out = accounts_payload(lang);
    auto snapshots = served_snapshots();
    std::vector<std::string> held;
    for (auto& [id, snap] : snapshots) {
        for (auto& p : snap.positions) {
            std::string symbol = to_upper(trim(p.symbol));
            if (!symbol.empty() && p.qty != 0.0) held.push_back(symbol);
        }
    }
    std::sort(held.begin(), held.end());
    held.erase(std::unique(held.begin(), held.end()), held.end());
    auto quotes = market_store_.get_stock_quotes(held);
    out["portfolio"] = portfolio_view(snapshots, accounts_, quotes);
    out["stock_quotes"] = json::object();
    for (auto& [symbol, quote] : quotes) {
        json q;
        q["last"] = quote.usable() ? json(*quote.last) : json(nullptr);
        q["trade_time"] = quote.trade_time ? json(iso_utc(*quote.trade_time)) : json(nullptr);
        q["observed_at"] = iso_utc(quote.observed_at);
        q["error"] = quote.error ? json(*quote.error) : json(nullptr);
        out["stock_quotes"][symbol] = q;
    }
    double now = wall_now();
    out["metas"] = json::array();
    for (const AccountMeta* meta : accounts_.by_num_id()) {
        json m = account_meta_view(*meta);
        auto it = snapshots.find(meta->id);
        m["has_snapshot"] = it != snapshots.end();
        m["stale"] = it != snapshots.end() && snapshot_is_stale(it->second.ts, now);
        out["metas"].push_back(m);
    }
    out["version"] = account_store_.version();
    out["generated_at"] = iso_utc(now);
    return out;
}

json ControlCenter::market_rows(const std::vector<std::string>& symbols) const {
    // Complete trade snapshots: last price, previous close and daily volume from the live quote feed.
    json out = json::object();
    for (auto& [symbol, row] : market_store_.get_market_rows(symbols)) out[symbol] = row.to_json();
    return out;
}

json ControlCenter::quotes_payload(const std::optional<std::string>& symbol) const {
    std::vector<std::string> requested;
    std::string clean = symbol ? to_upper(trim(*symbol)) : "";
    if (!clean.empty()) requested = {clean};
    else requested = config_.market_data_symbols();
    json out;
    out["rows"] = market_rows(requested);
    return out;
}

json ControlCenter::market_activity_payload(const std::string& symbol) const {
    if (!market_activity_ || !market_order_store_) throw std::runtime_error("market activity is unavailable");
    json feed = market_order_store_->feed_status();
    // Skipped invalid messages make our figures incomplete even while the feed is live.
    std::string state = feed.at("invalid_messages").get<long long>() ? std::string("incomplete") : feed.at("state").get<std::string>();
    json out = market_activity_->snapshot(symbol, accounts_, state);
    out["ok"] = true;
    out["symbols"] = config_.market_data_symbols();
    return out;
}

std::vector<std::string> ControlCenter::insight_symbols() const {
    std::set<std::string> all(symbols_.begin(), symbols_.end());
    for (auto& s : market_store_.get_book_symbols()) all.insert(s);
    return std::vector<std::string>(all.begin(), all.end());
}

json ControlCenter::insights_payload(const std::string& symbol, int depth) const {
    int max_levels = config_.market_data.market_insights_max_levels;
    int requested = std::max(1, std::min(depth, max_levels));
    OrderBookSnapshot book = market_store_.get_book(symbol, requested);
    json out = book.to_json();
    out["symbols"] = insight_symbols();
    return out;
}

json ControlCenter::smart_status() const {
    json out;
    out["ok"] = true;
    out["parents"] = json::array();
    if (smart_) {
        for (auto& p : smart_->list_parents()) out["parents"].push_back(p);
        json feeds = json::object();
        for (auto& [k, v] : smart_->feed_states()) feeds[k] = v;
        out["feeds"] = feeds;
    }
    out["available"] = smart_ != nullptr;
    return out;
}

json ControlCenter::smart_buying_status() const {
    json out;
    out["ok"] = true;
    out["parents"] = json::array();
    if (buying_) {
        for (auto& p : buying_->list_parents()) out["parents"].push_back(p);
        json feeds = json::object();
        for (auto& [k, v] : buying_->feed_states()) feeds[k] = v;
        out["feeds"] = feeds;
    }
    out["available"] = buying_ != nullptr;
    return out;
}

json ControlCenter::algo_status() const {
    json out;
    out["fast"] = json::array();
    if (fast_) {
        for (auto& s : fast_->statuses()) {
            json st;
            st["key"] = s.key;
            st["session_id"] = s.session_id;
            st["command"] = s.command;
            st["started_at"] = s.started_at;
            st["cycles"] = s.cycles;
            st["published_commands"] = s.published_commands;
            st["consecutive_capacity_noops"] = s.consecutive_capacity_noops;
            st["last_reason"] = s.last_reason;
            st["stopping"] = s.stopping;
            st["last_cycle"] = s.last_cycle;
            out["fast"].push_back(st);
        }
    }
    out["smart"] = smart_status();
    out["buying"] = smart_buying_status();
    out["active"] = any_algorithm_active();
    return out;
}

json ControlCenter::feeds_health() const {
    json out = json::object();
    out["accounts"] = feed_health_.accounts ? feed_health_.accounts() : json(nullptr);
    out["market"] = feed_health_.market ? feed_health_.market() : json(nullptr);
    out["orders"] = feed_health_.orders ? feed_health_.orders() : json(nullptr);
    out["volume"] = feed_health_.volume ? feed_health_.volume() : json(nullptr);
    out["buying_orders"] = feed_health_.buying_orders ? feed_health_.buying_orders() : json(nullptr);
    out["buying_volume"] = feed_health_.buying_volume ? feed_health_.buying_volume() : json(nullptr);
    out["own_orders"] = market_order_store_ ? market_order_store_->feed_status() : json(nullptr);
    auto last_account = account_store_.kafka_last_seen();
    out["account_store_last_update"] = last_account ? json(iso_utc(*last_account)) : json(nullptr);
    auto last_market = market_store_.last_update_wall();
    out["market_store_last_update"] = last_market ? json(iso_utc(*last_market)) : json(nullptr);
    return out;
}

json ControlCenter::health() const {
    json out;
    out["ok"] = true;
    out["version"] = NG_VERSION;
    out["started_at"] = iso_utc(started_at_);
    out["uptime_seconds"] = std::max(0.0, wall_now() - started_at_);
    out["server_time"] = iso_utc_now();
    out["feeds"] = feeds_health();
    out["commands"] = sink_ ? sink_->stats() : json(nullptr);
    out["live_connections"] = static_cast<long long>(const_cast<EventHub&>(hub_).subscriber_count());
    out["algorithms"] = algo_status();
    out["accounts_with_data"] = static_cast<long long>(served_snapshots().size());
    out["configured_accounts"] = static_cast<long long>(accounts_.size());
    out["operations_log"] = operations_log_ ? json(operations_log_->path()) : json(nullptr);
    out["persistence"] = persistence_status_ ? persistence_status_() : json(nullptr);
    return out;
}

json ControlCenter::commands_history() const {
    json out;
    out["ok"] = true;
    out["commands"] = json::array();
    if (sink_) {
        auto history = sink_->history();
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            json c;
            c["at"] = iso_utc(it->wall);
            c["key"] = it->key;
            c["command"] = it->command;
            c["status"] = it->status;
            c["error"] = it->error;
            out["commands"].push_back(c);
        }
    }
    return out;
}

json ControlCenter::bootstrap(const std::string& lang, const std::string& user) const {
    json out;
    out["ok"] = true;
    out["user"] = user;
    out["lang"] = lang;
    out["privileged"] = config_.is_privileged(user);
    out["can_access_admin_control"] = config_.can_access_admin_control(user);
    out["can_access_market_data"] = config_.can_access_market_data(user);
    out["can_manage_trading"] = config_.is_privileged(user);
    out["symbols"] = symbols_;
    out["market_data_symbols"] = config_.market_data_symbols();
    out["broker_orders_today"] = orders::broker_orders_today();
    out["accounts"] = json::array();
    for (const AccountMeta* meta : accounts_.by_num_id()) out["accounts"].push_back(account_meta_view(*meta));
    out["market_insights_max_levels"] = config_.market_data.market_insights_max_levels;
    out["smart_selling_available"] = smart_ != nullptr;
    out["smart_buying_available"] = buying_ != nullptr;
    out["fast_trading"] = json::object();
    out["fast_trading"]["book_level_choices"] = json::array({1, 2, 3, 4, 5});
    out["fast_trading"]["target_percentage_choices"] = json::array();
    for (int choice : orders::kFastTargetPercentageChoices) out["fast_trading"]["target_percentage_choices"].push_back(choice);
    out["fast_trading"]["cycle_seconds_choices"] = json::array({5, 10, 15, 30, 60});
    out["fast_trading"]["minimum_cycle_seconds_by_medium"] = config_.fast_trading.minimum_cycle_seconds_by_medium;
    out["limit_share_shortcuts"] = json::array({3000, 5000, 10000, 20000, 30000, 50000});
    out["through_market_choices"] = json::array({1, 5, 10, 20});
    out["quick_default_dollars"] = 10000;
    out["version"] = NG_VERSION;
    out["server_time"] = iso_utc_now();
    out["kafka"] = json{{"account_details_topic", config_.kafka.account_details_topic},
                        {"trading_commands_topic", config_.kafka.trading_commands_topic},
                        {"market_data_topic", config_.kafka.market_data_topic},
                        {"stock_quotes_topic", config_.kafka.stock_quotes_topic}};
    return out;
}

}  // namespace ng::web
