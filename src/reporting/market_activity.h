// Persistent five-minute analytics from quote counters and broker order snapshots
// (trading_ui/services/market_activity.py).
//
// Quote prices are samples, so VWAP is an estimate. Fills are attributed to the broker report time;
// ambiguous intervals and corrections are marked partial. History lives in the activity reporting
// database (`market_quote_samples`, `market_order_samples`): one day is retained, plus an older
// cumulative baseline for each order so replays never turn lifetime fills into new volume.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/json.h"
#include "core/sqlite.h"
#include "model/account.h"
#include "model/market_data.h"
#include "orders/order_status.h"

namespace ng::reporting {

class MarketActivity {
public:
    using Clock = std::function<double()>;  // wall-clock epoch seconds

    static constexpr int kWindowSeconds = 300;
    static constexpr int kQuoteMaxAge = 30;
    static constexpr int kHistorySeconds = 86400;
    static constexpr const char* kMarketZone = "America/New_York";

    explicit MarketActivity(const std::string& db_path, Clock clock = nullptr);
    ~MarketActivity();
    void close();

    // Stores one quote observation (duplicates by symbol and observation time are ignored).
    void record_quote(const StockQuote& quote);
    // Stores one cumulative fill snapshot per (account, order, sequence). Returns false for an
    // identical duplicate; throws std::invalid_argument for conflicting or reordered evidence.
    bool record_order(const orders::OrderEvent& event);
    // The current clock-aligned five-minute window and the previous three, newest first.
    json snapshot(const std::string& symbol, const AccountDirectory& accounts, const std::string& order_feed,
                  std::optional<double> now = std::nullopt);

    static long long window_start(double epoch);

private:
    Clock clock_;
    std::recursive_mutex mutex_;
    std::unique_ptr<Sqlite> db_;
    double next_prune_ = 0;
};

}  // namespace ng::reporting
