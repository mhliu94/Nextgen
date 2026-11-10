#include "algo/smart_buying/runtime.h"

#include <cmath>
#include <set>

#include "core/strings.h"
#include "core/time_util.h"
#include "orders/commands.h"

namespace ng::smart {

SmartBuyingRuntime::SmartBuyingRuntime(const SmartSellingConfig& config, MarketDataStore& market_data_store, MetasProvider metas,
                                       SnapshotsProvider snapshots, PublishCommand publish, OwnsAccount fast_owns_account,
                                       std::shared_ptr<SmartBuyingEngine> engine, Clock clock, Clock wall_clock,
                                       reporting::SessionReporter* reporting)
    : SmartSellingRuntime(config, std::move(metas), std::move(snapshots), std::move(publish), std::move(fast_owns_account),
                          engine, std::move(clock), std::move(wall_clock), reporting, "H",
                          [](const std::string& path, const Clock& c, const Clock& w) -> std::shared_ptr<SmartSellingEngine> {
                              return std::make_shared<SmartBuyingEngine>(path, c, w);
                          }),
      market_data_store_(market_data_store) {
    buying_engine_ = dynamic_cast<SmartBuyingEngine*>(engine_.get());
    if (!buying_engine_) throw std::invalid_argument("Smart buying requires a SmartBuyingEngine");
}

Decimal SmartBuyingRuntime::available_position(const std::string& account_id, const std::string&) {
    auto snapshots = snapshots_();
    auto it = snapshots.find(account_id);
    if (it == snapshots.end() || !it->second.ts || it->second.ts->empty()) throw ValueError("A recent USD cash snapshot is required");
    const AccountSnapshot& snapshot = it->second;
    auto parsed = parse_iso8601(*snapshot.ts);
    if (!parsed) throw ValueError("Account cash snapshot is stale");
    double age = wall_clock_() - parsed->epoch_seconds;
    if (!parsed->has_offset || !(-1 <= age && age <= config_.account_max_age)) throw ValueError("Account cash snapshot is stale");
    if (!snapshot.trading_enabled) throw ValueError("Trading is disabled for this account");
    double raw_cash = snapshot.cash;
    if (auto available = snapshot.available_cash_by_currency.find("USD"); available != snapshot.available_cash_by_currency.end()) {
        raw_cash = available->second;
    } else if (auto balance = snapshot.cash_by_currency.find("USD"); balance != snapshot.cash_by_currency.end()) {
        raw_cash = balance->second;
    }
    if (!std::isfinite(raw_cash)) throw ValueError("invalid USD cash");
    Decimal cash = Decimal::from_python_float(raw_cash);
    if (cash.digit_count() > 30 || cash.adjusted() > 24 || cash.exponent() < -24) {
        throw ValueError("USD cash exceeds supported decimal precision");
    }
    // max(Decimal(0), cash) keeps the plain zero for non-positive balances.
    if (!(cash > Decimal::zero())) cash = Decimal::zero();
    buying_engine_->set_cash_limit(account_id, cash);
    return cash;
}

json SmartBuyingRuntime::start_parent(const json& settings) {
    if (!settings.is_object()) throw ValueError("Smart buying settings must be an object");
    json accounts_raw = settings.contains("account_ids") ? settings.at("account_ids")
                                                         : json::array({settings.value("account_id", json(""))});
    if (!accounts_raw.is_array() || accounts_raw.empty()) throw ValueError("Select at least one account");
    std::vector<std::string> accounts;
    for (const auto& a : accounts_raw) {
        if (!a.is_string() || trim(a.get<std::string>()).empty()) throw ValueError("Select at least one account");
        accounts.push_back(trim(a.get<std::string>()));
    }
    const AccountDirectory& metas = metas_();
    {
        std::set<std::string> unique(accounts.begin(), accounts.end());
        bool known = true;
        for (const auto& a : accounts) known = known && metas.contains(a);
        if (unique.size() != accounts.size() || !known) throw ValueError("Invalid or duplicate account selection");
    }
    std::string symbol = settings.contains("symbol") && !settings.at("symbol").is_null()
                             ? canonical_symbol(json_to_string(&settings.at("symbol")).value_or(""))
                             : std::string();
    if (symbol.empty()) throw ValueError("A symbol is required");

    std::lock_guard lock(mutex_);
    std::vector<std::string> owned;
    for (const auto& a : accounts) {
        if (owns_account(a) || fast_owns_account_(a)) owned.push_back(a);
    }
    if (!owned.empty()) throw ValueError(orders::algo_account_conflict_error(owned));
    if (!feeds_live()) throw ValueError("Wait for live order-status and trading-volume feeds");
    OrderBookSnapshot book = market_data_store_.get_book(symbol, 1);
    double age = book.asof_epoch ? wall_clock_() - static_cast<double>(*book.asof_epoch) : 0.0;
    if (!book.asof_epoch || *book.asof_epoch == 0 || !(-1 <= age && age <= config_.quote_max_age) || book.asks.empty()) {
        throw ValueError("A fresh best ask is required to size the buying target");
    }
    std::map<std::string, std::string> rules = config_.rules_for(symbol);
    Decimal step = Decimal::parse(rules.at("quantity_step"));
    if (!(step > Decimal(1))) step = Decimal(1);
    Decimal minimum = Decimal::parse(rules.at("min_quantity"));
    if (!(minimum > Decimal(100))) minimum = Decimal(100);
    minimum = minimum.round_up_to_step(step);
    rules["min_quantity"] = minimum.str();
    rules["quantity_step"] = step.str();
    const std::optional<double>& best_ask = book.asks.front().price;
    if (!best_ask) throw ValueError("best ask must be a finite number");
    if (!std::isfinite(*best_ask)) throw ValueError("invalid best ask");
    Decimal reference_price =
        parse_input_decimal(json(python_float_repr(*best_ask)), "best ask", true).round_up_to_step(Decimal::parse(rules.at("tick_size")));

    std::map<std::string, Decimal> budgets, quantities;
    Decimal quantity;
    for (const auto& account : accounts) {
        if (feedback_->has_open_orders(account, symbol)) throw ValueError("Existing orders must reach terminal status first");
        Decimal cash = available_position(account, symbol);
        budgets[account] = cash;
        // floor((cash / price) / step) * step, computed exactly.
        Decimal affordable = Decimal(cash.floor_div(reference_price * step), 0) * step;
        quantities[account] = affordable >= minimum ? affordable : Decimal::zero();
        quantity += quantities[account];
    }
    if (quantity < minimum) throw ValueError("At least one selected account must afford " + minimum.str() + " shares");
    std::string mode = to_lower(json_to_string(json_find(settings, "mode")).value_or("normal"));
    if (settings.contains("mode") && settings.at("mode").is_null()) mode = "none";
    if (mode == "1") mode = "patient";
    else if (mode == "2") mode = "normal";
    else if (mode == "3") mode = "urgent";
    if (!buying_participation_percentages().count(mode)) throw ValueError("mode must be patient, normal, or urgent");

    json permitted = json::object();
    for (const char* key : {"start_time", "deadline", "maximum_price"}) {
        if (settings.contains(key)) permitted[key] = settings.at(key);
    }
    permitted["mode"] = mode;
    permitted["participation_percentage"] = buying_participation_percentages().at(mode).str();
    permitted["account_id"] = accounts.front();
    permitted["account_ids"] = accounts;
    json account_quantities = json::object(), account_budgets = json::object();
    for (const auto& account : accounts) {
        account_quantities[account] = quantities.at(account).str();
        account_budgets[account] = budgets.at(account).str();
    }
    permitted["account_quantities"] = account_quantities;
    permitted["account_budgets"] = account_budgets;
    permitted["quantity"] = quantity.str();
    permitted["symbol"] = symbol;
    json instrument_rules = json::object();
    for (auto& [key, value] : rules) instrument_rules[key] = value;
    permitted["instrument_rules"] = instrument_rules;
    permitted["quote_max_age"] = config_.quote_max_age;
    permitted["trade_max_age"] = config_.trade_max_age;
    json parent = engine_->start_parent(permitted);
    std::vector<json> started{parent};
    sync_reporting(&started, &permitted);
    return parent;
}

std::optional<json> SmartBuyingRuntime::prepare_submit(const json& action) {
    std::string client_id = action.at("client_order_id").get<std::string>();
    if (!feeds_live()) {
        engine_->abort_dispatch(client_id, "waiting for live order and volume feeds");
        return std::nullopt;
    }
    std::string account_id = action.at("account_id").get<std::string>();
    std::string symbol = action.at("symbol").get<std::string>();
    available_position(account_id, symbol);
    if (feedback_->has_open_orders(account_id, symbol)) {
        engine_->abort_dispatch(client_id, "conflicting open order", true);
        return std::nullopt;
    }
    // The engine rechecks stop/deadline, quote age, depth, price and budget atomically.
    return engine_->prepare_dispatch(client_id);
}

}  // namespace ng::smart
