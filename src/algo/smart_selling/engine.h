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
    json ask_size = nullptr;     // displayed size at the best ask (required by Smart buying)
    // Distinct displayed price levels with positive size; when absent only the best price is known.
    std::optional<std::vector<json>> bid_prices;
    std::optional<std::vector<json>> ask_prices;
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
    virtual ~SmartSellingEngine();
    void close();

    // "SELL" for Smart selling (mode G), "BUY" for Smart buying (mode H).
    virtual const char* side() const { return "SELL"; }

    json start_parent(const json& settings);
    virtual void on_quote(const std::string& symbol, const QuoteUpdate& update);
    // Refreshes the per-account average holding costs used to pick the selling account, without
    // changing an outstanding child's account. A missing or null cost is unknown, never zero.
    void set_account_costs(const std::string& parent_id, const std::map<std::string, std::optional<Decimal>>& costs);
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

protected:
    // Subclasses finish construction themselves so restart recovery runs with their overrides.
    struct DeferRecovery {};
    SmartSellingEngine(const std::string& db_path, Clock clock, Clock wall_clock, int max_active_parents, DeferRecovery);
    // Marks persisted intents as dispatched and puts every outstanding child into reconciliation.
    void recover();

    // --- side-specific policy (overridden by SmartBuyingEngine) ---------------------------------
    // True when executable liquidity is the ask side (buying); consumption is tracked on that side.
    virtual bool liquidity_is_ask() const { return false; }
    virtual const std::map<std::string, Decimal>& mode_percentages() const { return participation_percentages(); }
    virtual const std::vector<std::string>& mode_names() const { return participation_modes(); }
    virtual ChildPlan child_policy(const json& parent, const QuoteState& quote, double now) const {
        return plan_child_order(parent, quote, now);
    }
    // Side-specific budget settings are persisted before parent registration.
    virtual void configure_parent(json& parent, const json& settings);
    // Displayed-liquidity cap for one child; nullopt when the side has none.
    virtual std::optional<Decimal> depth_budget(const QuoteState& quote) const;
    // Limit price and aggressiveness to use at dispatch time.
    virtual std::pair<Decimal, bool> dispatch_price(const json& parent, const json& order, const QuoteState& quote) const;
    virtual Decimal account_capacity(const json& parent, const std::string& account_id, const Decimal& price,
                                     bool include_order = false) const;
    // Account indices in the order they should be tried for the next child.
    std::vector<std::size_t> account_selection_indices(const json& parent) const;
    virtual void save(const json& parent);
    virtual void book_cumulative_fill(json& parent, json& order, const Decimal& cumulative);
    virtual void finish_if_needed(json& parent, double now);
    virtual json action_dict(const json& parent, const std::string& kind);
    virtual json make_public(json parent);

    template <typename Fn>
    auto transaction(Fn&& fn) -> decltype(fn());

    std::vector<json> active(const std::optional<std::string>& symbol = std::nullopt);
    json load(const std::string& parent_id);
    static json upgrade_parent(json parent);
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
    void action(json& parent, const std::string& kind);
    void abort_unsent(json& parent, json& order, const std::string& reason, double now, bool stop_parent = false);
    void query(json& parent, double now);
    void cancel(json& parent, double now);
    void check_archived_report(json& parent, const json& archived, const std::string& status, const Decimal& cumulative,
                               const std::optional<long long>& sequence, bool authoritative);
    bool accept_cumulative_report(json& parent, json& order, const Decimal& cumulative,
                                  const std::optional<long long>& sequence);
    void acknowledge_order(json& parent, json& order, const std::string& status, const Decimal& cumulative, double now);
    void apply_terminal_report(json& parent, json& order, const std::string& status, const Decimal& cumulative, double now);
    void apply_working_report(json& parent, json& order, double now);
    void hold_conflict(json& parent, const std::string& reason);
    static bool deadline_reached(const json& parent, double now);
    std::string quote_reason(const json& parent, double now);
    static void latency(json& parent, const std::string& kind, double seconds);
    void diagnostic(json& parent, double now);
    void tick_parent(json& parent, double now);
    void service_working_order(json& parent, double now);
    std::optional<ChildPlan> plan_child(json& parent, double now);
    void reserve_child(json& parent, const ChildPlan& plan, double now);

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
