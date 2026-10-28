// Market data: price-book derived quotes, level-2 books, last-trade stock quotes and
// the historical close CSV, plus the thread-safe store that serves them.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/json.h"

namespace ng {

struct QuoteRow {
    std::string symbol;
    std::optional<double> prev_close;
    std::optional<double> last;
    std::optional<double> change;
    std::optional<double> change_pct;
    std::optional<double> short_interest;
    std::optional<long long> volume;
    std::optional<long long> asof_epoch;
    std::optional<std::string> error;
    json to_json() const;
};

struct StockQuote {
    std::string symbol;
    std::optional<double> last;
    std::optional<double> trade_time;
    double observed_at = 0.0;
    std::optional<bool> is_delayed;
    std::optional<bool> is_halted;
    std::optional<std::string> error;
    bool usable() const;  // last present, finite, positive and no error
};

struct BookLevel {
    std::optional<double> price;
    std::optional<double> quantity;
    std::optional<long long> order_count;
    json to_json() const;
};

struct OrderBookSnapshot {
    std::string symbol;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
    std::optional<long long> asof_epoch;
    int depth_limit = 20;
    std::optional<std::string> error;
    json to_json() const;
};

// Pure parsers (quote_row_from_price_book / order_book_from_price_book / stock_quote_from_payload).
std::optional<QuoteRow> quote_row_from_price_book(const json& payload);
std::optional<OrderBookSnapshot> order_book_from_price_book(const json& payload, int depth_limit = 20);
std::optional<StockQuote> stock_quote_from_payload(const json& payload);
// Sort helpers exposed for tests: most aggressive first, missing prices last.
std::vector<BookLevel> top_levels(const std::vector<BookLevel>& levels, int depth_limit, bool is_bid);

class MarketDataStore {
public:
    using Listener = std::function<void(const std::string& symbol)>;

    void upsert(const QuoteRow& row);
    void upsert_book(OrderBookSnapshot book);
    void upsert_stock_quote(const StockQuote& quote);

    std::map<std::string, QuoteRow> get_for_symbols(const std::vector<std::string>& symbols) const;
    OrderBookSnapshot get_book(const std::string& symbol, int depth_limit = 20) const;
    std::vector<std::string> get_book_symbols() const;
    std::map<std::string, StockQuote> get_stock_quotes(const std::vector<std::string>& symbols) const;
    std::map<std::string, StockQuote> get_all_stock_quotes() const;

    // Monotonic change counters for cheap change detection.
    std::uint64_t quotes_version() const;
    std::uint64_t book_version(const std::string& symbol) const;
    std::uint64_t books_version() const;
    std::optional<double> last_update_wall() const;

    void add_quote_listener(Listener listener);
    void add_book_listener(Listener listener);

private:
    void notify(const std::vector<Listener>& listeners, const std::string& symbol);

    mutable std::shared_mutex mutex_;
    std::map<std::string, QuoteRow> rows_;
    std::map<std::string, OrderBookSnapshot> books_;
    std::map<std::string, StockQuote> stock_quotes_;
    std::map<std::string, std::uint64_t> book_versions_;
    std::uint64_t quotes_version_ = 0;
    std::uint64_t books_version_ = 0;
    std::optional<double> last_update_wall_;
    std::mutex listeners_mutex_;
    std::vector<Listener> quote_listeners_;
    std::vector<Listener> book_listeners_;
};

// Latest close per symbol from historical_prices.csv (symbol,trade_date,close); reloads on mtime change.
class HistoricalCloseStore {
public:
    explicit HistoricalCloseStore(std::string csv_path);
    std::optional<double> get_prev_close(const std::string& symbol) const;
    std::map<std::string, double> all() const;
    const std::string& path() const { return path_; }

private:
    struct Row {
        std::string trade_date;
        double close;
    };
    void reload_if_needed() const;
    std::map<std::string, Row> load_latest_rows() const;

    std::string path_;
    mutable std::mutex mutex_;
    mutable std::optional<std::int64_t> mtime_ns_;
    mutable std::map<std::string, Row> rows_;
};

}  // namespace ng
