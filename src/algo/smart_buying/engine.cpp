#include "algo/smart_buying/engine.h"

#include <stdexcept>

namespace ng::smart {
namespace {

Decimal D(const json& object, const char* key) { return parse_decimal(object.at(key)); }
std::string S(const Decimal& value) { return decimal_string(value); }

}  // namespace

SmartBuyingEngine::SmartBuyingEngine(const std::string& db_path, Clock clock, Clock wall_clock, int max_active_parents)
    : SmartSellingEngine(db_path, std::move(clock), std::move(wall_clock), max_active_parents, DeferRecovery{}) {
    // Restart recovery saves every active parent; run it here so the cash-budget invariant applies.
    recover();
}

void SmartBuyingEngine::set_cash_limit(const std::string& account_id, const Decimal& cash) {
    std::lock_guard lock(cash_mutex_);
    cash_limits_[account_id] = cash;
}

std::map<std::string, Decimal> SmartBuyingEngine::cash_limits() const {
    std::lock_guard lock(cash_mutex_);
    return cash_limits_;
}

void SmartBuyingEngine::configure_parent(json& parent, const json& settings) {
    const json* ceiling = json_find(settings, "maximum_price");
    bool has_ceiling = ceiling && !ceiling->is_null() && !(ceiling->is_string() && ceiling->get<std::string>().empty());
    parent["maximum_price"] = has_ceiling ? json(S(parse_input_decimal(*ceiling, "maximum_price", true))) : json(nullptr);
    if (D(parent, "min_quantity") < Decimal(100)) throw ValueError("Smart buying requires a minimum of 100 shares");
    // Rates are fixed per urgency; an API request cannot override them.
    parent["participation"] =
        Decimal::python_div(buying_participation_percentages().at(parent.at("mode").get<std::string>()), Decimal(100)).str();
    // No horizon-derived slices, partial flush timers, or progress escalation.
    parent["child_cap"] = parent.at("quantity");
    parent["base_child_cap"] = parent.at("quantity");
    const json& budgets = settings.at("account_budgets");
    for (auto it = parent["accounts"].begin(); it != parent["accounts"].end(); ++it) {
        it.value()["cash_budget"] = S(parse_input_decimal(budgets.at(it.key()), "cash budget", false, true));
        it.value()["spent"] = "0";
    }
}

void SmartBuyingEngine::on_quote(const std::string& symbol, const QuoteUpdate& update) {
    QuoteUpdate adjusted = update;
    if (update.ask_size.is_null() || parse_decimal(update.ask_size, "quantity", false, true) <= Decimal::zero()) {
        adjusted.healthy = false;
    }
    SmartSellingEngine::on_quote(symbol, adjusted);
}

std::optional<Decimal> SmartBuyingEngine::depth_budget(const QuoteState& quote) const {
    return Decimal::max(Decimal::zero(), quote.ask_size - quote.consumed);
}

std::pair<Decimal, bool> SmartBuyingEngine::dispatch_price(const json&, const json& order, const QuoteState&) const {
    // Keep the planned limit fixed, including if the ask moves while queued.
    return {D(order, "limit_price"), true};
}

Decimal SmartBuyingEngine::account_capacity(const json& parent, const std::string& account_id, const Decimal& price,
                                            bool include_order) const {
    const json& account = parent.at("accounts").at(account_id);
    Decimal shares = SmartSellingEngine::account_capacity(parent, account_id, price, include_order);
    Decimal limit;
    {
        std::lock_guard lock(cash_mutex_);
        auto it = cash_limits_.find(account_id);
        if (it != cash_limits_.end()) limit = it->second;
    }
    Decimal cash = Decimal::max(Decimal::zero(), Decimal::min(D(account, "cash_budget") - D(account, "spent"), limit));
    // floor((cash / price) / step) * step == floor(cash / (price * step)) * step, computed exactly.
    Decimal step = D(parent, "quantity_step");
    Decimal affordable = Decimal(cash.floor_div(price * step), 0) * step;
    return Decimal::min(shares, affordable);
}

void SmartBuyingEngine::book_cumulative_fill(json& parent, json& order, const Decimal& cumulative) {
    Decimal delta = cumulative - D(order, "cumulative_filled");
    json& account = parent["accounts"][order.at("account_id").get<std::string>()];
    // The limit price is a conservative upper bound on spend; do not depend on delayed account
    // snapshots or recycle apparent cash after a partial fill.
    account["spent"] = S(D(account, "spent") + delta * D(order, "limit_price"));
    SmartSellingEngine::book_cumulative_fill(parent, order, cumulative);
}

void SmartBuyingEngine::save(const json& parent) {
    const json& order = parent.at("order");
    for (auto it = parent.at("accounts").begin(); it != parent.at("accounts").end(); ++it) {
        const json& account = it.value();
        Decimal spent = D(account, "spent");
        Decimal reserved_cash;
        if (!order.is_null() && order.at("account_id").get<std::string>() == it.key()) {
            reserved_cash = D(account, "R") * D(order, "limit_price");
        }
        if (spent.is_negative() || spent + reserved_cash > D(account, "cash_budget")) {
            throw std::logic_error("Smart buying cash budget invariant violated");
        }
    }
    SmartSellingEngine::save(parent);
}

void SmartBuyingEngine::finish_if_needed(json& parent, double now) {
    SmartSellingEngine::finish_if_needed(parent, now);
    if (!parent.at("active").get<bool>() || !parent.at("order").is_null()) return;
    Decimal minimum = D(parent, "min_quantity");
    for (auto it = parent.at("accounts").begin(); it != parent.at("accounts").end(); ++it) {
        if (!(D(it.value(), "quantity") - D(it.value(), "F") < minimum)) return;
    }
    parent["active"] = false;
    parent["state"] = "COMPLETED";
    parent["reason"] = "remaining target below 100-share minimum";
}

json SmartBuyingEngine::action_dict(const json& parent, const std::string& kind) {
    json action = SmartSellingEngine::action_dict(parent, kind);
    action["trading_mode"] = "H";
    return action;
}

json SmartBuyingEngine::make_public(json parent) {
    json result = SmartSellingEngine::make_public(parent);
    result["maximum_price"] = parent.at("maximum_price");
    result["trading_mode"] = "H";
    for (auto it = parent.at("accounts").begin(); it != parent.at("accounts").end(); ++it) {
        result["accounts"][it.key()]["cash_budget"] = it.value().at("cash_budget");
        result["accounts"][it.key()]["spent"] = it.value().at("spent");
    }
    return result;
}

}  // namespace ng::smart
