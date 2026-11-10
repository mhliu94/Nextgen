// Conservative quote and trading-volume adapters for POV execution (smart_selling_market.py).
// Price-book messages refresh depth only; the trading-volume topic supplies cumulative
// observations whose fresh, ordered differences earn participation credit.
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "core/decimal.h"
#include "core/json.h"
#include "algo/smart_selling/engine.h"

namespace ng::smart {

struct MarketStatus {
    std::string state;
    std::string reason;
};

class SmartSellingMarketAdapter {
public:
    using Clock = std::function<double()>;

    SmartSellingMarketAdapter(SmartSellingEngine& engine, Clock clock = nullptr, Clock wall_clock = nullptr,
                              double max_source_age = 10, double rate_window_seconds = 300, int max_observations = 256,
                              double resume_cooldown_seconds = 10);

    void set_connected(bool connected);
    void set_volume_connected(bool connected);
    MarketStatus market_status(const std::string& symbol);
    std::optional<Decimal> volume_rate(const std::string& symbol);
    bool apply_price_book(const json& payload, const std::optional<std::string>& topic = std::nullopt,
                          std::optional<int> partition = std::nullopt, std::optional<long long> offset = std::nullopt);
    bool apply_volume(const json& payload, const std::optional<std::string>& topic = std::nullopt,
                      std::optional<int> partition = std::nullopt, std::optional<long long> offset = std::nullopt);

private:
    struct SymbolState {
        std::string book_feed_id;
        std::optional<double> book_time;
        std::optional<double> book_snapshot_time;
        std::optional<long long> book_sequence;
        std::optional<long long> raw_sequence;
        bool quote_gap = false;
        bool reset_quote = true;
        bool quote_healthy = false;

        std::string volume_feed_id;
        std::string session;
        bool volume_healthy = false;
        std::optional<Decimal> volume;
        std::optional<double> volume_time;
        std::optional<long long> volume_sequence;
        std::string volume_mode;

        std::string market_state = "UNKNOWN";
        std::string market_reason = "halt and real-time status not yet confirmed";
        bool halt_latched = false;
        std::optional<double> resume_started;
        std::optional<double> resume_source;
        int resume_count = 0;
        std::string status_feed_id;
        std::optional<long long> status_sequence;
        std::optional<double> status_time;

        std::string credited_session;
        std::optional<Decimal> credited_cumulative;
        std::optional<double> credited_at;
        bool counter_conflict = false;

        std::deque<std::tuple<double, double, Decimal>> observations;
        std::deque<std::string> seen_trades;
        std::map<std::string, bool> seen_trade_index;
    };

    struct VolumeEnvelope {
        std::string symbol;
        std::string feed_id;
        long long sequence = 0;
        double observed_at = 0.0;
        std::string session;
        std::optional<Decimal> total_volume;
    };

    bool fresh(const std::optional<double>& timestamp) const;
    SymbolState& symbol_state(const std::string& symbol);
    void save_checkpoint(const std::string& symbol, SymbolState& state);
    static void reset_volume(SymbolState& state);
    bool volume_is_healthy(const SymbolState& state) const;
    void refresh_health(const std::string& symbol, SymbolState& state);
    bool halt_gate(const std::string& symbol, SymbolState& state, const json& payload, double source);
    bool accept_offset(const std::optional<std::string>& topic, std::optional<int> partition, std::optional<long long> offset);
    void observation(SymbolState& state, double start, double end, const Decimal& quantity);
    std::optional<VolumeEnvelope> parse_volume_envelope(const std::string& symbol, const json& payload);
    bool accept_volume_status(SymbolState& state, const VolumeEnvelope& envelope, const json& payload);
    void check_credited_watermark(SymbolState& state, const VolumeEnvelope& envelope);
    bool prepare_volume_baseline(SymbolState& state, const VolumeEnvelope& envelope, const json& payload);
    void update_blocked_baseline(SymbolState& state, const VolumeEnvelope& envelope, const json& payload);
    void admit_volume(SymbolState& state, const VolumeEnvelope& envelope, const json& payload);
    void cumulative(const std::string& symbol, SymbolState& state, const json& payload, double source, long long sequence);
    static std::string event_id(const std::string& symbol, const SymbolState& state, const std::optional<long long>& sequence);
    void unknown_correction(const std::string& symbol, SymbolState& state, const Decimal& delta, double source,
                            const std::optional<long long>& sequence);
    void trades(const std::string& symbol, SymbolState& state, const json& trades, bool corrections_only = false);

    SmartSellingEngine& engine_;
    Clock clock_;
    Clock wall_clock_;
    double max_source_age_;
    double rate_window_seconds_;
    int max_observations_;
    double resume_cooldown_seconds_;
    std::recursive_mutex mutex_;
    std::map<std::string, SymbolState> symbols_;
    std::map<std::pair<std::string, int>, long long> offsets_;
    bool connected_ = true;
    bool volume_connected_ = true;
    std::optional<double> volume_connected_since_;
};

// Parsing helpers shared with tests.
std::optional<Decimal> market_decimal(const json* value);
std::optional<double> market_timestamp(const json* value, bool nanoseconds = false);
std::optional<long long> market_sequence(const json* value);
// Best price and summed same-price depth from a price-book side.
std::pair<std::optional<Decimal>, Decimal> best_level(const json* side, bool bid);
// Distinct prices of levels with actual, positive displayed liquidity, ascending.
std::vector<Decimal> displayed_prices(const json* side);

}  // namespace ng::smart
