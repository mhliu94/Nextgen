// Smart buying (mode H): simple cash-limited POV buying on Smart selling's durable order/volume
// ledger (trading_ui/services/smart_buying.py).
//
// The engine buys at a fixed share of newly observed volume with best-ask limits. It reuses the
// selling engine's credit ledger (E = C + R + F + D), order correlation, replay protection and
// reconciliation, and replaces only the side-specific policy: sizing, pricing, account capacity
// (shares and cash) and completion.
#pragma once

#include <map>
#include <mutex>
#include <string>

#include "algo/smart_selling/engine.h"

namespace ng::smart {

class SmartBuyingEngine : public SmartSellingEngine {
public:
    explicit SmartBuyingEngine(const std::string& db_path, Clock clock = nullptr, Clock wall_clock = nullptr,
                               int max_active_parents = -1);

    const char* side() const override { return "BUY"; }
    // A quote without a positive displayed ask size cannot be bought from: it is recorded unhealthy.
    void on_quote(const std::string& symbol, const QuoteUpdate& update) override;

    // Latest available USD cash per account, refreshed by the runtime before every decision. An
    // account without an entry has no spendable cash.
    void set_cash_limit(const std::string& account_id, const Decimal& cash);
    std::map<std::string, Decimal> cash_limits() const;

protected:
    bool liquidity_is_ask() const override { return true; }
    const std::map<std::string, Decimal>& mode_percentages() const override { return buying_participation_percentages(); }
    const std::vector<std::string>& mode_names() const override { return buying_participation_modes(); }
    ChildPlan child_policy(const json& parent, const QuoteState& quote, double now) const override {
        return plan_buy(parent, quote, now);
    }
    void configure_parent(json& parent, const json& settings) override;
    std::optional<Decimal> depth_budget(const QuoteState& quote) const override;
    std::pair<Decimal, bool> dispatch_price(const json& parent, const json& order, const QuoteState& quote) const override;
    Decimal account_capacity(const json& parent, const std::string& account_id, const Decimal& price,
                             bool include_order = false) const override;
    void save(const json& parent) override;
    void book_cumulative_fill(json& parent, json& order, const Decimal& cumulative) override;
    void finish_if_needed(json& parent, double now) override;
    json action_dict(const json& parent, const std::string& kind) override;
    json make_public(json parent) override;

private:
    mutable std::mutex cash_mutex_;
    std::map<std::string, Decimal> cash_limits_;
};

}  // namespace ng::smart
