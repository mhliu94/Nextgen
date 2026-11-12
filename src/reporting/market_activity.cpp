#include "reporting/market_activity.h"

#include <cmath>
#include <filesystem>
#include <map>
#include <vector>

#include "core/decimal.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "core/time_zone.h"

namespace ng::reporting {
namespace {

struct QuoteSample {
    double observed = 0;
    std::optional<double> price;
    std::optional<long long> volume;
    std::optional<double> trade_time;
    std::optional<long long> last_size;
    bool error = false;
};

struct OrderSample {
    std::string account_id, order_id, side;
    long long sequence = 0;
    double created = 0, updated = 0;
    Decimal filled;
};

struct Window {
    long long start = 0, end = 0;
    bool current = false;
    std::optional<long long> market_volume;
    std::optional<double> vwap_estimate;
    bool market_complete = false;
    Decimal bought, sold;
    bool our_complete = false;
};

QuoteSample read_quote(const Statement& row) {
    // Columns: symbol, observed, price, volume, trade_time, last_size, error.
    QuoteSample sample;
    sample.observed = row.column_double(1);
    if (!row.is_null(2)) sample.price = row.column_double(2);
    if (!row.is_null(3)) sample.volume = row.column_int(3);
    if (!row.is_null(4)) sample.trade_time = row.column_double(4);
    if (!row.is_null(5)) sample.last_size = row.column_int(5);
    sample.error = !row.is_null(6) && !row.column_text(6).empty();
    return sample;
}

std::string market_day(double epoch) {
    static const TimeZone zone = *TimeZone::find(MarketActivity::kMarketZone);
    return zone.date(epoch);
}

void market_window(const std::vector<QuoteSample>& quotes, Window& window, double end) {
    double start = static_cast<double>(window.start);
    const QuoteSample* baseline = nullptr;
    for (auto it = quotes.rbegin(); it != quotes.rend(); ++it) {
        if (it->observed <= start) {
            baseline = &*it;
            break;
        }
    }
    const QuoteSample* previous = baseline;
    long long total = 0;
    Decimal weighted;
    bool complete = baseline && !baseline->error && baseline->volume && start - baseline->observed <= MarketActivity::kQuoteMaxAge;
    bool observed = complete;
    for (const QuoteSample& quote : quotes) {
        if (!(start < quote.observed && quote.observed <= end)) continue;
        bool valid = previous && !previous->error && !quote.error && previous->volume && quote.volume && quote.price &&
                     quote.observed - previous->observed <= MarketActivity::kQuoteMaxAge &&
                     market_day(quote.observed) == market_day(previous->observed) && *quote.volume >= *previous->volume;
        if (valid) {
            long long delta = *quote.volume - *previous->volume;
            total += delta;
            // Python's default Decimal context (28 digits) applies to each operation.
            Decimal term = (Decimal::from_python_float(*quote.price) * Decimal(delta)).round_significant(28);
            weighted = (weighted + term).round_significant(28);
            observed = true;
        } else {
            complete = false;
        }
        previous = &quote;
    }
    if (!previous || previous->error || end - previous->observed > MarketActivity::kQuoteMaxAge) complete = false;
    window.market_volume = observed ? std::optional<long long>(total) : std::nullopt;
    window.vwap_estimate = total ? std::optional<double>(Decimal::python_div(weighted, Decimal(total)).to_double()) : std::nullopt;
    window.market_complete = complete;
}

void our_windows(std::vector<Window>& windows, const std::vector<OrderSample>& orders) {
    // Samples arrive ordered by account, order and sequence.
    std::size_t index = 0;
    while (index < orders.size()) {
        std::size_t end = index;
        while (end < orders.size() && orders[end].account_id == orders[index].account_id && orders[end].order_id == orders[index].order_id) {
            ++end;
        }
        struct Lot {
            double at;
            Decimal quantity;
            std::string side;
        };
        std::vector<Lot> lots;
        Decimal previous_quantity;
        double previous_time = orders[index].created;
        for (std::size_t i = index; i < end; ++i) {
            const OrderSample& order = orders[i];
            Decimal delta = order.filled - previous_quantity;
            double at = order.updated;
            if (delta.is_positive()) {
                lots.push_back({at, delta, order.side});
                // The stream has no execution time for each fill: an increase spanning window
                // boundaries is uncertain for every window it may belong to.
                if (MarketActivity::window_start(previous_time) != MarketActivity::window_start(at)) {
                    for (Window& window : windows) {
                        if (previous_time < static_cast<double>(window.end) && at >= static_cast<double>(window.start)) {
                            window.our_complete = false;
                        }
                    }
                }
            } else if (delta.is_negative()) {
                // Cumulative corrections reduce prior allocations. Without execution IDs their exact
                // windows cannot be established.
                Decimal remaining = -delta;
                for (auto lot = lots.rbegin(); lot != lots.rend(); ++lot) {
                    Decimal removed = Decimal::min(lot->quantity, remaining);
                    lot->quantity -= removed;
                    remaining -= removed;
                    if (!removed.is_zero()) {
                        for (Window& window : windows) {
                            if (static_cast<double>(window.start) <= lot->at && lot->at < static_cast<double>(window.end)) {
                                window.our_complete = false;
                            }
                        }
                    }
                    if (remaining.is_zero()) break;
                }
            }
            previous_quantity = order.filled;
            previous_time = at;
        }
        for (const Lot& lot : lots) {
            for (Window& window : windows) {
                if (static_cast<double>(window.start) <= lot.at && lot.at < static_cast<double>(window.end)) {
                    (lot.side == "BUY" ? window.bought : window.sold) += lot.quantity;
                }
            }
        }
        index = end;
    }
}

}  // namespace

long long MarketActivity::window_start(double epoch) {
    return static_cast<long long>(std::floor(epoch / kWindowSeconds)) * kWindowSeconds;
}

MarketActivity::MarketActivity(const std::string& db_path, Clock clock) : clock_(clock ? std::move(clock) : Clock(&wall_now)) {
    if (db_path != ":memory:") {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(db_path).parent_path(), ec);
    }
    db_ = std::make_unique<Sqlite>(db_path, Sqlite::Mode::ReadWriteCreate, 10000);
    db_->exec("PRAGMA journal_mode=WAL");
    // Samples are re-derived from Kafka replay after a crash; skip the per-commit fsync.
    db_->exec("PRAGMA synchronous=NORMAL");
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS market_quote_samples (
            symbol TEXT NOT NULL, observed REAL NOT NULL, price REAL,
            volume INTEGER, trade_time REAL, last_size INTEGER, error TEXT,
            PRIMARY KEY(symbol, observed)
        );
        CREATE TABLE IF NOT EXISTS market_order_samples (
            account_id TEXT NOT NULL, order_id TEXT NOT NULL, sequence INTEGER NOT NULL,
            created REAL NOT NULL, updated REAL NOT NULL, symbol TEXT NOT NULL,
            side TEXT NOT NULL, filled TEXT NOT NULL,
            PRIMARY KEY(account_id, order_id, sequence)
        );
        CREATE INDEX IF NOT EXISTS market_order_symbol_time
            ON market_order_samples(symbol, updated);
    )SQL");
}

MarketActivity::~MarketActivity() { close(); }

void MarketActivity::close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        db_->close();
        db_.reset();
    }
}

void MarketActivity::record_quote(const StockQuote& quote) {
    double now = clock_();
    if (!(now - kHistorySeconds <= quote.observed_at && quote.observed_at <= now + 1)) return;
    std::lock_guard lock(mutex_);
    if (!db_) return;
    db_->begin_immediate();
    try {
        Statement insert(*db_, "INSERT OR IGNORE INTO market_quote_samples VALUES (?,?,?,?,?,?,?)");
        insert.bind(1, quote.symbol).bind(2, quote.observed_at);
        if (quote.last) insert.bind(3, *quote.last); else insert.bind_null(3);
        if (quote.volume) insert.bind(4, *quote.volume); else insert.bind_null(4);
        if (quote.trade_time) insert.bind(5, *quote.trade_time); else insert.bind_null(5);
        if (quote.last_size) insert.bind(6, *quote.last_size); else insert.bind_null(6);
        insert.bind_optional(7, quote.error);
        insert.exec();
        if (now >= next_prune_) {
            Statement quotes(*db_, "DELETE FROM market_quote_samples WHERE observed < ?");
            quotes.bind(1, now - kHistorySeconds);
            quotes.exec();
            // Retain one old cumulative baseline for each order. Replays do not turn an order's
            // lifetime fills into new trading volume.
            Statement samples(*db_, "DELETE FROM market_order_samples AS old WHERE updated < ? "
                                    "AND sequence < (SELECT MAX(new.sequence) FROM market_order_samples AS new "
                                    "WHERE new.account_id=old.account_id AND new.order_id=old.order_id AND new.updated < ?)");
            samples.bind(1, now - kHistorySeconds).bind(2, now - kHistorySeconds);
            samples.exec();
            next_prune_ = now + kWindowSeconds;
        }
        db_->commit();
    } catch (...) {
        db_->rollback();
        throw;
    }
}

bool MarketActivity::record_order(const orders::OrderEvent& event) {
    const json& data = event.payload;
    std::string account_id = data.at("account_id").get<std::string>();
    std::string order_id = data.at("order_id").get<std::string>();
    const json& raw_sequence = data.at("sequence");
    long long sequence = raw_sequence.is_number_float() ? static_cast<long long>(raw_sequence.get<double>()) : raw_sequence.get<long long>();
    std::string symbol = canonical_symbol(data.at("symbol").get<std::string>());
    std::string side = data.at("side").get<std::string>();
    const json& raw_filled = data.at("filled_quantity");
    // str(Decimal(str(filled)).normalize()), exactly as the Python service stores it.
    Decimal filled_value = raw_filled.is_number_float() ? Decimal::from_python_float(raw_filled.get<double>())
                                                        : Decimal::parse(raw_filled.dump());
    std::string filled = filled_value.python_normalized_str();
    std::lock_guard lock(mutex_);
    if (!db_) throw std::runtime_error("market activity is closed");
    db_->begin_immediate();
    try {
        bool duplicate = false;
        // A retained baseline already covers older reports outside the analytics
        // window. Replaying those obsolete versions must not make today's own
        // volume incomplete (legacy publishers sometimes restarted sequence 0).
        if (event.updated_at < clock_() - kHistorySeconds) {
            Statement baseline(*db_, "SELECT sequence,created,updated,symbol,side FROM market_order_samples "
                                     "WHERE account_id=? AND order_id=? ORDER BY sequence DESC LIMIT 1");
            baseline.bind(1,account_id).bind(2,order_id);
            if (baseline.step() && baseline.column_int(0) > sequence &&
                baseline.column_double(1) == event.created_at && baseline.column_double(2) >= event.updated_at &&
                baseline.column_text(3) == symbol && baseline.column_text(4) == side) {
                db_->commit();
                return false;
            }
        }
        {
            Statement existing(*db_, "SELECT created,updated,symbol,side,filled FROM market_order_samples "
                                     "WHERE account_id=? AND order_id=? AND sequence=?");
            existing.bind(1, account_id).bind(2, order_id).bind(3, sequence);
            if (existing.step()) {
                if (existing.column_double(0) != event.created_at || existing.column_double(1) != event.updated_at ||
                    existing.column_text(2) != symbol || existing.column_text(3) != side || existing.column_text(4) != filled) {
                    throw std::invalid_argument("Conflicting order volume snapshot");
                }
                duplicate = true;
            }
        }
        if (!duplicate) {
            for (bool earlier : {true, false}) {
                Statement neighbor(*db_, std::string("SELECT created,updated,symbol,side FROM market_order_samples "
                                                     "WHERE account_id=? AND order_id=? AND sequence ") +
                                             (earlier ? "< ? ORDER BY sequence DESC LIMIT 1" : "> ? ORDER BY sequence ASC LIMIT 1"));
                neighbor.bind(1, account_id).bind(2, order_id).bind(3, sequence);
                if (!neighbor.step()) continue;
                if (neighbor.column_double(0) != event.created_at || neighbor.column_text(2) != symbol || neighbor.column_text(3) != side) {
                    throw std::invalid_argument("Order identity changed");
                }
                double neighbor_updated = neighbor.column_double(1);
                if ((earlier && neighbor_updated > event.updated_at) || (!earlier && neighbor_updated < event.updated_at)) {
                    throw std::invalid_argument("Order report time decreased");
                }
            }
            Statement insert(*db_, "INSERT INTO market_order_samples VALUES (?,?,?,?,?,?,?,?)");
            insert.bind(1, account_id).bind(2, order_id).bind(3, sequence).bind(4, event.created_at).bind(5, event.updated_at);
            insert.bind(6, symbol).bind(7, side).bind(8, filled);
            insert.exec();
        }
        db_->commit();
        return !duplicate;
    } catch (...) {
        db_->rollback();
        throw;
    }
}

json MarketActivity::snapshot(const std::string& symbol, const AccountDirectory& accounts, const std::string& order_feed,
                              std::optional<double> now_in) {
    double now = now_in ? *now_in : clock_();
    long long current = window_start(now);
    long long oldest = current - 3 * kWindowSeconds;
    std::vector<QuoteSample> quotes;
    std::vector<OrderSample> orders;
    std::optional<QuoteSample> latest;
    {
        std::lock_guard lock(mutex_);
        if (!db_) throw std::runtime_error("market activity is closed");
        db_->begin();
        try {
            {
                Statement rows(*db_, "SELECT * FROM market_quote_samples WHERE symbol=? AND observed>=? AND observed<=? ORDER BY observed");
                rows.bind(1, symbol).bind(2, static_cast<double>(oldest - kQuoteMaxAge)).bind(3, now);
                while (rows.step()) quotes.push_back(read_quote(rows));
            }
            {
                // Include earlier snapshots of orders active in these windows, so repeated status
                // messages never count the same fills twice.
                Statement rows(*db_, "SELECT sample.account_id,sample.order_id,sample.sequence,sample.created,sample.updated,"
                                     "sample.side,sample.filled FROM market_order_samples sample "
                                     "WHERE sample.symbol=? AND sample.updated<=? AND EXISTS ("
                                     "SELECT 1 FROM market_order_samples recent "
                                     "WHERE recent.account_id=sample.account_id AND recent.order_id=sample.order_id "
                                     "AND recent.updated>=? AND recent.updated<=?) "
                                     "ORDER BY sample.account_id,sample.order_id,sample.sequence");
                rows.bind(1, canonical_symbol(symbol)).bind(2, now).bind(3, static_cast<double>(oldest)).bind(4, now);
                while (rows.step()) {
                    std::string account_id = rows.column_text(0);
                    if (!accounts.contains(account_id)) continue;
                    OrderSample sample;
                    sample.account_id = std::move(account_id);
                    sample.order_id = rows.column_text(1);
                    sample.sequence = rows.column_int(2);
                    sample.created = rows.column_double(3);
                    sample.updated = rows.column_double(4);
                    sample.side = rows.column_text(5);
                    sample.filled = Decimal::try_parse(rows.column_text(6)).value_or(Decimal::zero());
                    orders.push_back(std::move(sample));
                }
            }
            {
                Statement row(*db_, "SELECT * FROM market_quote_samples WHERE symbol=? AND observed<=? ORDER BY observed DESC LIMIT 1");
                row.bind(1, symbol).bind(2, now);
                if (row.step()) latest = read_quote(row);
            }
        } catch (...) {
            db_->rollback();
            throw;
        }
        db_->rollback();
    }
    bool feed_live = order_feed == "live";
    std::vector<Window> windows(4);
    for (int i = 0; i < 4; ++i) {
        Window& window = windows[static_cast<std::size_t>(i)];
        window.start = current - static_cast<long long>(i) * kWindowSeconds;
        window.end = current + static_cast<long long>(1 - i) * kWindowSeconds;
        window.current = i == 0;
        market_window(quotes, window, std::min(now, static_cast<double>(window.end)));
        window.our_complete = feed_live;
    }
    our_windows(windows, orders);
    bool live = latest && !latest->error && now - latest->observed <= kQuoteMaxAge;
    json out;
    out["symbol"] = symbol;
    out["generated_at"] = now;
    out["timezone"] = kMarketZone;
    out["order_feed"] = order_feed;
    out["quote_state"] = live ? "live" : "unavailable";
    json last_trade;
    last_trade["time"] = live && latest->trade_time ? json(*latest->trade_time) : json(nullptr);
    last_trade["quantity"] = live && latest->last_size ? json(*latest->last_size) : json(nullptr);
    last_trade["price"] = live && latest->price ? json(*latest->price) : json(nullptr);
    out["last_trade"] = last_trade;
    out["windows"] = json::array();
    bool our_unknown = !feed_live && orders.empty();
    for (const Window& window : windows) {
        json w;
        w["start"] = window.start;
        w["end"] = window.end;
        w["current"] = window.current;
        w["market_volume"] = window.market_volume ? json(*window.market_volume) : json(nullptr);
        w["vwap_estimate"] = window.vwap_estimate ? json(*window.vwap_estimate) : json(nullptr);
        w["market_complete"] = window.market_complete;
        w["bought"] = our_unknown ? json(nullptr) : json(window.bought.to_double());
        w["sold"] = our_unknown ? json(nullptr) : json(window.sold.to_double());
        w["gross"] = our_unknown ? json(nullptr) : json((window.bought + window.sold).to_double());
        w["our_complete"] = window.our_complete;
        out["windows"].push_back(w);
    }
    return out;
}

}  // namespace ng::reporting
