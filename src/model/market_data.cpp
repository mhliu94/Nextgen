#include "model/market_data.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng {

namespace {

json opt_to_json(const std::optional<double>& v) { return v ? json(*v) : json(nullptr); }
json opt_to_json(const std::optional<long long>& v) { return v ? json(*v) : json(nullptr); }
json opt_to_json(const std::optional<std::string>& v) { return v ? json(*v) : json(nullptr); }

std::optional<double> to_float(const json* v) {
    if (!v || v->is_null()) return std::nullopt;
    return json_to_double(v);
}

std::optional<long long> to_int(const json* v) {
    if (!v || v->is_null()) return std::nullopt;
    return json_to_int(v);
}

std::optional<double> quote_timestamp(const json* value) {
    auto text = json_to_string(value);
    if (!text) return std::nullopt;
    auto parsed = parse_iso8601(*text);
    if (!parsed || !parsed->has_offset) return std::nullopt;
    return parsed->epoch_seconds;
}

std::vector<BookLevel> extract_levels(const json* side, int depth_limit, bool is_bid) {
    std::vector<BookLevel> out;
    if (!side || !side->is_object()) return out;
    const json* levels = json_find(*side, "levels");
    if (!levels || !levels->is_array()) return out;
    for (const auto& level : *levels) {
        if (!level.is_object()) continue;
        BookLevel bl;
        bl.price = to_float(json_first_present(level, "prc", "price", "Price"));
        bl.quantity = to_float(json_first_present(level, "qty", "total_qty", "quantity", "TotalSize"));
        bl.order_count = to_int(json_first_present(level, "order_count", "num_orders", "TotalOrderCount"));
        out.push_back(bl);
    }
    return top_levels(out, depth_limit, is_bid);
}

std::optional<double> best_price(const json* side, bool is_bid) {
    auto levels = extract_levels(side, 1, is_bid);
    if (levels.empty()) return std::nullopt;
    return levels[0].price;
}

std::optional<long long> asof_from_payload(const json& payload) {
    auto ns = to_int(json_find(payload, "update_time"));
    if (!ns) return std::nullopt;
    return static_cast<long long>(static_cast<double>(*ns) / 1e9);
}

}  // namespace

std::vector<BookLevel> top_levels(const std::vector<BookLevel>& levels, int depth_limit, bool is_bid) {
    std::vector<BookLevel> sorted = levels;
    std::stable_sort(sorted.begin(), sorted.end(), [is_bid](const BookLevel& a, const BookLevel& b) {
        if (!a.price.has_value() || !b.price.has_value()) return a.price.has_value() && !b.price.has_value();
        return is_bid ? *a.price > *b.price : *a.price < *b.price;
    });
    int cap = std::max(0, depth_limit);
    if (static_cast<int>(sorted.size()) > cap) sorted.resize(static_cast<std::size_t>(cap));
    return sorted;
}

json QuoteRow::to_json() const {
    json out;
    out["symbol"] = symbol;
    out["prev_close"] = opt_to_json(prev_close);
    out["last"] = opt_to_json(last);
    out["change"] = opt_to_json(change);
    out["change_pct"] = opt_to_json(change_pct);
    out["short_interest"] = opt_to_json(short_interest);
    out["volume"] = opt_to_json(volume);
    out["asof_epoch"] = opt_to_json(asof_epoch);
    out["error"] = opt_to_json(error);
    return out;
}

bool StockQuote::usable() const { return !error && last && std::isfinite(*last) && *last > 0; }

json BookLevel::to_json() const {
    json out;
    out["price"] = opt_to_json(price);
    out["quantity"] = opt_to_json(quantity);
    out["order_count"] = opt_to_json(order_count);
    return out;
}

json OrderBookSnapshot::to_json() const {
    json out;
    out["symbol"] = symbol;
    out["depth_limit"] = depth_limit;
    out["asof_epoch"] = opt_to_json(asof_epoch);
    out["error"] = opt_to_json(error);
    out["bids"] = json::array();
    for (auto& l : bids) out["bids"].push_back(l.to_json());
    out["asks"] = json::array();
    for (auto& l : asks) out["asks"].push_back(l.to_json());
    return out;
}

std::optional<QuoteRow> quote_row_from_price_book(const json& payload) {
    if (!payload.is_object()) return std::nullopt;
    std::string symbol = to_upper(trim(json_to_string(json_find(payload, "symbol")).value_or("")));
    if (symbol.empty()) return std::nullopt;
    auto bid = best_price(json_find(payload, "bid_side"), true);
    auto ask = best_price(json_find(payload, "offer_side"), false);
    QuoteRow row;
    row.symbol = symbol;
    if (bid && ask) row.last = (*bid + *ask) / 2.0;
    else if (bid) row.last = bid;
    else if (ask) row.last = ask;
    row.asof_epoch = asof_from_payload(payload);
    row.volume = to_int(json_find(payload, "volume"));
    if (!row.volume) row.volume = to_int(json_find(payload, "day_volume"));
    if (!row.volume) row.volume = to_int(json_find(payload, "total_volume"));
    row.short_interest = to_float(json_find(payload, "short_interest"));
    if (!row.short_interest) row.short_interest = to_float(json_find(payload, "short_interest_pct"));
    return row;
}

std::optional<OrderBookSnapshot> order_book_from_price_book(const json& payload, int depth_limit) {
    if (!payload.is_object()) return std::nullopt;
    std::string symbol = to_upper(trim(json_to_string(json_find(payload, "symbol")).value_or("")));
    if (symbol.empty()) return std::nullopt;
    int cap = std::max(1, std::min(depth_limit, 20));
    OrderBookSnapshot book;
    book.symbol = symbol;
    book.bids = extract_levels(json_find(payload, "bid_side"), cap, true);
    book.asks = extract_levels(json_find(payload, "offer_side"), cap, false);
    book.asof_epoch = asof_from_payload(payload);
    book.depth_limit = cap;
    return book;
}

std::optional<StockQuote> stock_quote_from_payload(const json& payload) {
    if (!payload.is_object()) return std::nullopt;
    const json* type = json_find(payload, "type");
    const json* version = json_find(payload, "schema_version");
    if (!type || !type->is_string() || type->get<std::string>() != "STOCK_QUOTE") return std::nullopt;
    if (!version || !version->is_number_integer() || version->get<long long>() != 1) return std::nullopt;
    std::string symbol = to_upper(trim(json_to_string(json_find(payload, "symbol")).value_or("")));
    if (symbol.empty()) return std::nullopt;
    auto observed = quote_timestamp(json_find(payload, "observed_at"));
    if (!observed) return std::nullopt;
    StockQuote quote;
    quote.symbol = symbol;
    quote.observed_at = *observed;
    if (const json* d = json_find(payload, "is_delayed"); d && d->is_boolean()) quote.is_delayed = d->get<bool>();
    if (const json* h = json_find(payload, "is_halted"); h && h->is_boolean()) quote.is_halted = h->get<bool>();
    quote.error = "last trade unavailable";
    const json* healthy = json_find(payload, "healthy");
    bool is_healthy = healthy && healthy->is_boolean() && healthy->get<bool>();
    if (!is_healthy || !quote.is_delayed.has_value() || *quote.is_delayed) return quote;
    const json* last_raw = json_find(payload, "last");
    if (!last_raw || last_raw->is_boolean()) return quote;
    auto last = json_to_double(last_raw);
    auto trade_time = quote_timestamp(json_find(payload, "trade_time"));
    if (!last || !trade_time || !std::isfinite(*last) || *last <= 0 || *trade_time > *observed + 1) return quote;
    quote.last = last;
    quote.trade_time = trade_time;
    quote.error.reset();
    if (const json* previous = json_find(payload, "prev_close"); previous && !previous->is_boolean()) {
        std::optional<double> close;
        if (previous->is_number()) close = previous->get<double>();
        else if (previous->is_string()) close = python_float(previous->get<std::string>());
        if (close && std::isfinite(*close) && *close > 0) quote.prev_close = close;
    }
    auto whole_number = [&](const char* key) -> std::optional<long long> {
        const json* value = json_find(payload, key);
        if (!value || !(value->is_number_integer() || value->is_number_unsigned())) return std::nullopt;
        if (value->is_number_unsigned() && value->get<unsigned long long>() > 9223372036854775807ULL) return std::nullopt;
        long long number = value->get<long long>();
        if (number < 0) return std::nullopt;
        return number;
    };
    quote.volume = whole_number("volume");
    quote.last_size = whole_number("last_size");
    return quote;
}

// ---------------------------------------------------------------------------
// MarketDataStore
// ---------------------------------------------------------------------------

void MarketDataStore::notify(const std::vector<Listener>& listeners, const std::string& symbol) {
    for (auto& l : listeners) l(symbol);
}

void MarketDataStore::upsert(const QuoteRow& row) {
    {
        std::unique_lock lock(mutex_);
        auto it = rows_.find(row.symbol);
        if (it == rows_.end()) {
            rows_[row.symbol] = row;
        } else {
            QuoteRow& existing = it->second;
            QuoteRow merged;
            merged.symbol = row.symbol;
            merged.prev_close = row.prev_close ? row.prev_close : existing.prev_close;
            merged.last = row.last ? row.last : existing.last;
            merged.change = row.change ? row.change : existing.change;
            merged.change_pct = row.change_pct ? row.change_pct : existing.change_pct;
            merged.short_interest = row.short_interest ? row.short_interest : existing.short_interest;
            merged.volume = row.volume ? row.volume : existing.volume;
            merged.asof_epoch = row.asof_epoch ? row.asof_epoch : existing.asof_epoch;
            merged.error = row.error ? row.error : existing.error;
            existing = merged;
        }
        ++quotes_version_;
        last_update_wall_ = wall_now();
    }
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(listeners_mutex_);
        listeners = quote_listeners_;
    }
    notify(listeners, row.symbol);
}

void MarketDataStore::upsert_book(OrderBookSnapshot book) {
    std::string symbol = book.symbol;
    {
        std::unique_lock lock(mutex_);
        books_[symbol] = std::move(book);
        ++book_versions_[symbol];
        ++books_version_;
        last_update_wall_ = wall_now();
    }
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(listeners_mutex_);
        listeners = book_listeners_;
    }
    notify(listeners, symbol);
}

void MarketDataStore::upsert_stock_quote(const StockQuote& quote) {
    bool changed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = stock_quotes_.find(quote.symbol);
        if (it == stock_quotes_.end() || quote.observed_at >= it->second.observed_at) {
            stock_quotes_[quote.symbol] = quote;
            ++quotes_version_;
            last_update_wall_ = wall_now();
            changed = true;
        }
    }
    if (!changed) return;
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(listeners_mutex_);
        listeners = quote_listeners_;
    }
    notify(listeners, quote.symbol);
}

std::map<std::string, QuoteRow> MarketDataStore::get_for_symbols(const std::vector<std::string>& symbols) const {
    std::shared_lock lock(mutex_);
    std::map<std::string, QuoteRow> out;
    for (auto& symbol : symbols) {
        auto it = rows_.find(symbol);
        if (it != rows_.end()) {
            out[symbol] = it->second;
            continue;
        }
        QuoteRow row;
        row.symbol = symbol;
        row.error = "no data yet";
        out[symbol] = row;
    }
    return out;
}

OrderBookSnapshot MarketDataStore::get_book(const std::string& symbol, int depth_limit) const {
    int cap = std::max(1, std::min(depth_limit, 20));
    std::shared_lock lock(mutex_);
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        OrderBookSnapshot empty;
        empty.symbol = symbol;
        empty.depth_limit = cap;
        empty.error = "no data yet";
        return empty;
    }
    OrderBookSnapshot out;
    out.symbol = it->second.symbol;
    out.bids = top_levels(it->second.bids, cap, true);
    out.asks = top_levels(it->second.asks, cap, false);
    out.asof_epoch = it->second.asof_epoch;
    out.depth_limit = cap;
    out.error = it->second.error;
    return out;
}

std::vector<std::string> MarketDataStore::get_book_symbols() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> out;
    for (auto& [k, v] : books_) out.push_back(k);
    return out;
}

std::map<std::string, StockQuote> MarketDataStore::get_stock_quotes(const std::vector<std::string>& symbols) const {
    std::shared_lock lock(mutex_);
    std::map<std::string, StockQuote> out;
    for (auto& symbol : symbols) {
        auto it = stock_quotes_.find(symbol);
        if (it != stock_quotes_.end()) out[symbol] = it->second;
    }
    return out;
}

std::map<std::string, StockQuote> MarketDataStore::get_all_stock_quotes() const {
    std::shared_lock lock(mutex_);
    return stock_quotes_;
}

std::map<std::string, QuoteRow> MarketDataStore::get_market_rows(const std::vector<std::string>& symbols, double max_age_seconds,
                                                                 std::optional<double> now_in) const {
    double now = now_in ? *now_in : wall_now();
    auto quotes = get_stock_quotes(symbols);
    std::map<std::string, QuoteRow> rows;
    for (const auto& symbol : symbols) {
        QuoteRow row;
        row.symbol = symbol;
        auto it = quotes.find(symbol);
        if (it == quotes.end()) {
            row.error = "no live quote yet";
        } else {
            const StockQuote& quote = it->second;
            row.error = quote.error;
            if (now - quote.observed_at > max_age_seconds) row.error = "quote feed is stale";
            bool usable = !row.error || row.error->empty();
            if (usable) {
                row.last = quote.last;
                row.prev_close = quote.prev_close;
                row.volume = quote.volume;
                if (row.last && row.prev_close) {
                    row.change = *row.last - *row.prev_close;
                    row.change_pct = *row.change / *row.prev_close * 100;
                }
            }
            row.asof_epoch = static_cast<long long>(quote.observed_at);
        }
        rows[symbol] = row;
    }
    return rows;
}

std::uint64_t MarketDataStore::quotes_version() const {
    std::shared_lock lock(mutex_);
    return quotes_version_;
}

std::uint64_t MarketDataStore::book_version(const std::string& symbol) const {
    std::shared_lock lock(mutex_);
    auto it = book_versions_.find(symbol);
    return it == book_versions_.end() ? 0 : it->second;
}

std::uint64_t MarketDataStore::books_version() const {
    std::shared_lock lock(mutex_);
    return books_version_;
}

std::optional<double> MarketDataStore::last_update_wall() const {
    std::shared_lock lock(mutex_);
    return last_update_wall_;
}

void MarketDataStore::add_quote_listener(Listener listener) {
    std::lock_guard lock(listeners_mutex_);
    quote_listeners_.push_back(std::move(listener));
}

void MarketDataStore::add_book_listener(Listener listener) {
    std::lock_guard lock(listeners_mutex_);
    book_listeners_.push_back(std::move(listener));
}

}  // namespace ng
