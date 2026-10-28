// Durable fixed-POV liquidation controller (trading_ui/services/smart_selling.py).
//
// Parents and child orders are JSON documents persisted in SQLite with the same schema
// and field layout as the Python engine, so an existing journal can be opened by this
// server. Every saved parent satisfies E = C + R + F + D and 0 <= F + R <= Q0.
// The outbox is at-most-once dispatched: ambiguous delivery is reconciled by client
// order id, never resubmitted automatically.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/decimal.h"
#include "core/json.h"
#include "core/sqlite.h"
#include "algo/smart_selling/policy.h"

namespace ng::smart {

// Python ValueError equivalent for invalid inputs or lifecycle violations.
using ValueError = std::invalid_argument;
// Python KeyError equivalent for unknown parent ids.
class KeyError : public std::out_of_range {
public:
    using std::out_of_range::out_of_range;
};

struct QuoteUpdate {
    json bid;
    json ask;
    json bid_size;
    json source_time = nullptr;  // epoch number, ISO string with offset, or null
    std::optional<long long> sequence;
    bool healthy = true;
    bool accessible = true;
    std::optional<double> received_at;
    bool reset_sequence = false;
    bool preserve_consumption = false;
};

struct TradeUpdate {
    json quantity;
    std::string event_id;
    json source_time = nullptr;
    std::optional<long long> sequence;
    std::optional<std::string> correction_of;
    bool healthy = true;
    std::optional<double> received_at;
    json interval_start = nullptr;
};

struct OrderReport {
    std::string status;
    json cumulative_filled;
    std::optional<std::string> broker_order_id;
    std::optional<long long> sequence;
    bool authoritative = true;
    bool final = false;
    std::optional<double> now;
};

class SmartSellingEngine {
public:
    using Clock = std::function<double()>;

    explicit SmartSellingEngine(const std::string& db_path, Clock clock = nullptr, Clock wall_clock = nullptr,
                                int max_active_parents = 100);
    ~SmartSellingEngine();
    void close();

    json start_parent(const json& settings);
    void on_quote(const std::string& symbol, const QuoteUpdate& update);
    void set_feed_health(const std::string& symbol, bool healthy, const std::string& reason = "", bool reset_sequence = false);
    void on_trade(const std::string& symbol, const TradeUpdate& trade);
    std::optional<json> prepare_dispatch(const std::string& client_order_id);
    bool abort_dispatch(const std::string& client_order_id, const std::string& reason, bool stop_parent = false);
    bool hold_order(const std::string& client_order_id, const std::string& reason);
    std::vector<json> drain_actions();
    void on_action_error(const std::string& client_order_id, const std::string& action, const std::string& error,
                         bool definitive = false, std::optional<double> now = std::nullopt);
    void on_order_report(const std::string& client_order_id, const OrderReport& report);
    json request_stop(const std::string& parent_id, std::optional<double> now = std::nullopt,
                      const std::string& reason = "manual stop");
    void set_remaining_volume_forecast(const std::string& parent_id, const std::optional<Decimal>& quantity);
    std::vector<json> tick(std::optional<double> now = std::nullopt);

    std::optional<json> get_market_checkpoint(const std::string& symbol);
    void set_market_checkpoint(const std::string& symbol, const json& checkpoint);
    std::optional<json> get_order_context(const std::string& client_order_id);
    std::optional<json> find_order_by_broker_id(const std::string& account_id, const std::string& broker_order_id);
    json get_parent(const std::string& parent_id);
    std::vector<json> list_parents(bool active_only = false, int limit = 100);
    std::vector<json> events(const std::string& parent_id, int limit = 100);

    // Exposed for tests.
    const std::map<std::string, QuoteState>& quotes_for_test() const { return quotes_; }

private:
    template <typename Fn>
    auto transaction(Fn&& fn) -> decltype(fn());

    std::vector<json> active(const std::optional<std::string>& symbol = std::nullopt);
    json load(const std::string& parent_id);
    static json upgrade_parent(json parent);
    void save(const json& parent);
    void save_order(const std::string& parent_id, const json& order);
    void event(const json& parent, const std::string& kind, json detail = json::object());
    void trim(json& parent);
    void register_parent(const json& parent);
    std::vector<json> tape_parents(const std::string& symbol, const std::optional<std::string>& correction_of);
    std::optional<Decimal> new_trade_rate(json& parent, const std::string& event_id, double now, double wall,
                                          const std::optional<double>& source, const std::optional<long long>& sequence,
                                          bool healthy, const std::optional<double>& interval_start);
    std::optional<Decimal> correction_rate(json& parent, const std::string& event_id, const std::string& correction_of,
                                           const Decimal& quantity);
    void credit_tape(json& parent, const std::string& event_id, const Decimal& quantity, const Decimal& rate,
                     const std::optional<std::string>& correction_of, const std::optional<double>& source, double now);
    json action_dict(const json& parent, const std::string& kind);
    void action(json& parent, const std::string& kind);
    void abort_unsent(json& parent, json& order, const std::string& reason, double now, bool stop_parent = false);
    void query(json& parent, double now);
    void cancel(json& parent, double now);
    void check_archived_report(json& parent, const json& archived, const std::string& status, const Decimal& cumulative,
                               const std::optional<long long>& sequence, bool authoritative);
    bool accept_cumulative_report(json& parent, json& order, const Decimal& cumulative,
                                  const std::optional<long long>& sequence);
    void book_cumulative_fill(json& parent, json& order, const Decimal& cumulative);
    void acknowledge_order(json& parent, json& order, const std::string& status, const Decimal& cumulative, double now);
    void apply_terminal_report(json& parent, json& order, const std::string& status, const Decimal& cumulative, double now);
    void apply_working_report(json& parent, json& order, double now);
    void hold_conflict(json& parent, const std::string& reason);
    static bool deadline_reached(const json& parent, double now);
    void finish_if_needed(json& parent, double now);
    std::string quote_reason(const json& parent, double now);
    static void latency(json& parent, const std::string& kind, double seconds);
    void diagnostic(json& parent, double now);
    void tick_parent(json& parent, double now);
    void service_working_order(json& parent, double now);
    std::optional<ChildPlan> plan_child(json& parent, double now);
    void reserve_child(json& parent, const ChildPlan& plan, double now);
    json make_public(json parent);

    Clock clock_;
    Clock wall_clock_;
    int max_active_parents_;
    std::recursive_mutex mutex_;
    std::unique_ptr<Sqlite> db_;
    std::map<std::string, QuoteState> quotes_;
};

// Helpers shared with the adapter/feedback modules.
inline const std::vector<std::string>& terminal_order_statuses() {
    static const std::vector<std::string> v = {"FILLED", "CANCELED", "CANCELLED", "REJECTED", "EXPIRED"};
    return v;
}
inline const std::vector<std::string>& working_order_statuses() {
    static const std::vector<std::string> v = {"NEW", "OPEN", "LIVE", "ACCEPTED", "PARTIALLY_FILLED", "PENDING_CANCEL"};
    return v;
}
bool is_terminal_status(const std::string& status);
bool is_working_status(const std::string& status);

}  // namespace ng::smart
