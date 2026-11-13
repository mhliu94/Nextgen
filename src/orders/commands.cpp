#include "orders/commands.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/strings.h"
#include "core/decimal.h"
#include "core/equity_price.h"
#include "core/time_util.h"
#include "core/time_zone.h"

namespace ng::orders {

std::string iso_utc_now() { return ng::iso_utc_now(); }

std::string new_command_id(const std::string& prefix) {
    return prefix + "_" + iso_utc_now() + "_" + std::to_string(epoch_micros());
}

double nearest_tick_price(const json& value, const std::string& tick_size) {
    try {
        if (value.is_boolean() || (!value.is_number() && !value.is_string()))
            throw std::invalid_argument("Limit price must be a positive number");
        Decimal price=value.is_string() ? Decimal::parse(value.get<std::string>()) :
            value.is_number_float() ? Decimal::from_double(value.get<double>()) : Decimal::parse(value.dump());
        Decimal tick=Decimal::parse(tick_size);
        if (!price.is_positive() || !tick.is_positive()) throw std::invalid_argument("Limit price and tick size must be positive");
        Decimal rounded=nearest_equity_price(price,tick);
        if (!rounded.is_positive()) throw std::invalid_argument("Limit price rounds below the minimum configured tick");
        return rounded.to_double();
    } catch (const DecimalError&) {
        throw std::invalid_argument("Limit price or tick size is invalid");
    }
}

namespace {

void add_broker_metadata(json& cmd, const std::string& account_id, const AccountDirectory& accounts) {
    if (const AccountMeta* meta = accounts.find(account_id)) {
        cmd["broker"] = meta->broker;
        cmd["broker_id"] = meta->broker_id;
    }
}

json base_command(const char* type, const std::string& prefix, const std::string& account_id) {
    json cmd;
    cmd["type"] = type;
    cmd["command_id"] = new_command_id(prefix);
    cmd["ts"] = iso_utc_now();
    cmd["account_id"] = account_id;
    return cmd;
}

bool valid_side(const std::string& side) { return side == "BUY" || side == "SELL"; }

std::string clean_symbol(const std::string& symbol) { return to_upper(trim(symbol)); }

}  // namespace

std::optional<double> safe_float(const std::optional<std::string>& raw) {
    if (!raw) return std::nullopt;
    std::string s = trim(*raw);
    if (s.empty()) return std::nullopt;
    std::optional<double> value = python_float(s);
    if (!value) throw std::invalid_argument("invalid number");
    if (!std::isfinite(*value)) throw std::invalid_argument("number must be finite");
    return value;
}

std::optional<long long> safe_int(const std::optional<std::string>& raw) {
    if (!raw) return std::nullopt;
    std::string s = trim(*raw);
    if (s.empty()) return std::nullopt;
    std::size_t idx = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    if (idx >= s.size()) throw std::invalid_argument("invalid integer");
    for (std::size_t i = idx; i < s.size(); ++i) {
        if (s[i] == '_' && i > idx && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i - 1]))) continue;
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) throw std::invalid_argument("invalid integer");
    }
    std::string digits;
    for (char c : s) if (c != '_') digits.push_back(c);
    try {
        return std::stoll(digits);
    } catch (...) {
        throw std::invalid_argument("invalid integer");
    }
}

json build_market_order_command(const std::string& account_id, const std::string& symbol, const std::string& side,
                                std::optional<long long> shares, std::optional<double> dollar_amount,
                                const AccountDirectory& accounts) {
    json cmd = base_command("MARKET_ORDER", "cmd", account_id);
    cmd["symbol"] = symbol;
    cmd["side"] = side;
    add_broker_metadata(cmd, account_id, accounts);
    if (shares) cmd["qty_shares"] = *shares;
    if (dollar_amount) cmd["notional_usd"] = *dollar_amount;
    return cmd;
}

json build_limit_order_command(const std::string& account_id, const std::string& symbol, const std::string& side,
                               long long shares, double limit_price, const AccountDirectory& accounts) {
    json cmd = base_command("LIMIT_ORDER", "limit", account_id);
    cmd["symbol"] = symbol;
    cmd["side"] = side;
    cmd["qty_shares"] = shares;
    cmd["limit_price"] = limit_price;
    add_broker_metadata(cmd, account_id, accounts);
    return cmd;
}

json build_limit_order_fok_command(const std::string& account_id, const std::string& symbol, const std::string& side,
                                   long long shares, double limit_price, const AccountDirectory& accounts) {
    json cmd = build_limit_order_command(account_id, symbol, side, shares, limit_price, accounts);
    cmd["type"] = "LIMIT_ORDER_FOK";
    cmd["time_in_force"] = "FOK";
    cmd["cancel_unfilled"] = true;
    return cmd;
}

json build_cancel_open_orders_command(const std::string& account_id, const AccountDirectory& accounts,
                                      const std::optional<std::string>& symbol) {
    json cmd = base_command("CANCEL_OPEN_ORDERS", "cancel_open", account_id);
    std::string clean = symbol ? clean_symbol(*symbol) : "";
    if (!clean.empty()) cmd["symbol"] = clean;
    add_broker_metadata(cmd, account_id, accounts);
    return cmd;
}

json build_currency_conversion_command(const std::string& account_id, const std::string& source_currency,
                                       const std::string& target_currency, double source_amount,
                                       const AccountDirectory& accounts) {
    json cmd = base_command("CURRENCY_CONVERSION", "fx", account_id);
    cmd["source_currency"] = source_currency;
    cmd["target_currency"] = target_currency;
    cmd["source_amount"] = source_amount;
    add_broker_metadata(cmd, account_id, accounts);
    return cmd;
}

json build_trading_status_command(const std::string& account_id, bool trading_enabled, const AccountDirectory& accounts) {
    json cmd = base_command("SET_TRADING_ENABLED", "trading_status", account_id);
    cmd["trading_enabled"] = trading_enabled;
    add_broker_metadata(cmd, account_id, accounts);
    return cmd;
}

json build_algo_start_command(const std::string& trading_mode, const std::string& symbol, double price_limit,
                              const std::vector<std::string>& account_ids, int book_levels, int target_percentage,
                              int cycle_seconds, bool test_mode, std::optional<long long> total_target_position) {
    if (trading_mode != "E" && trading_mode != "F") throw std::invalid_argument("Algorithmic trading mode must be E or F.");
    json cmd;
    cmd["type"] = "START_ALGO_TRADING";
    cmd["command_id"] = new_command_id("algo");
    cmd["ts"] = iso_utc_now();
    cmd["trading_mode"] = trading_mode;
    cmd["symbol"] = symbol;
    cmd["fast_trading_price_limit"] = price_limit;
    cmd["fast_trading_account_ids"] = account_ids;
    cmd["fast_trading_book_levels"] = book_levels;
    cmd["fast_trading_target_percentage"] = target_percentage;
    cmd["fast_trading_cycle_seconds"] = cycle_seconds;
    cmd["fast_trading_test_mode"] = test_mode;
    if (total_target_position) cmd["fast_trading_total_target_position"] = *total_target_position;
    return cmd;
}

std::string broker_orders_today() {
    return TimeZone::find(kBrokerOrdersTimezone)->date(wall_now());
}

Validated validate_broker_orders_inputs(const std::string& trading_date_raw, const std::vector<std::string>& account_ids,
                                        const AccountDirectory& accounts, const BrokerOrdersMessages& m,
                                        const std::optional<std::string>& today) {
    // Strict YYYY-MM-DD: date.fromisoformat() accepts more spellings, which are then rejected by the
    // isoformat() round-trip check.
    int year = 0, month = 0, day = 0;
    bool shaped = trading_date_raw.size() == 10 && trading_date_raw[4] == '-' && trading_date_raw[7] == '-';
    for (std::size_t i = 0; shaped && i < trading_date_raw.size(); ++i) {
        if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(trading_date_raw[i]))) shaped = false;
    }
    if (!shaped || !parse_civil_date(trading_date_raw, year, month, day) || year < 1) return {std::nullopt, m.invalid_date};
    if (trading_date_raw > today.value_or(broker_orders_today())) return {std::nullopt, m.future_date};
    if (account_ids.empty()) return {std::nullopt, m.accounts_required};
    bool all = false;
    for (const auto& id : account_ids) {
        if (id == "__ALL__") all = true;
        else if (!accounts.contains(id)) return {std::nullopt, m.invalid_account};
    }
    std::set<std::string> selected;
    if (all) {
        for (const auto& meta : accounts.ordered()) selected.insert(meta.id);
    } else {
        selected.insert(account_ids.begin(), account_ids.end());
    }
    if (selected.empty()) return {std::nullopt, m.accounts_required};
    std::vector<std::string> ordered(selected.begin(), selected.end());
    std::stable_sort(ordered.begin(), ordered.end(), [&](const std::string& a, const std::string& b) {
        int na = accounts.find(a)->num_id, nb = accounts.find(b)->num_id;
        return na != nb ? na < nb : a < b;
    });
    json cmd;
    cmd["type"] = "GET_BROKER_ORDERS";
    cmd["command_id"] = "broker_orders_" + uuid4_hex();
    cmd["ts"] = iso_utc_now();
    cmd["trading_date"] = trading_date_raw;
    cmd["timezone"] = kBrokerOrdersTimezone;
    cmd["account_ids"] = ordered;
    return {cmd, ""};
}

json build_algo_stop_command(const std::optional<std::string>& reason) {
    json cmd;
    cmd["type"] = "STOP_ALGO_TRADING";
    cmd["command_id"] = new_command_id("algo_stop");
    cmd["ts"] = iso_utc_now();
    cmd["scope"] = "ALL";
    std::string clean = reason ? trim(*reason) : "";
    if (!clean.empty()) cmd["reason"] = clean;
    return cmd;
}

Validated validate_order_inputs(const std::string& account_id, const std::string& symbol, const std::string& side,
                                const std::optional<std::string>& shares_raw, const std::optional<std::string>& dollars_raw,
                                const AccountDirectory& accounts, const OrderMessages& m) {
    if (!accounts.contains(account_id)) return {std::nullopt, m.invalid_account};
    std::string sym = clean_symbol(symbol);
    if (sym.empty()) return {std::nullopt, m.invalid_symbol};
    if (!valid_side(side)) return {std::nullopt, m.invalid_side};
    std::optional<long long> shares;
    std::optional<double> dollars;
    try {
        shares = safe_int(shares_raw);
    } catch (const std::exception&) {
        return {std::nullopt, m.shares_positive};
    }
    try {
        dollars = safe_float(dollars_raw);
    } catch (const std::exception&) {
        return {std::nullopt, m.dollars_positive};
    }
    if (shares && dollars) return {std::nullopt, m.both_shares_and_dollars};
    if (!shares && !dollars) return {std::nullopt, m.neither_shares_nor_dollars};
    if (shares && *shares <= 0) return {std::nullopt, m.shares_positive};
    if (dollars && *dollars <= 0) return {std::nullopt, m.dollars_positive};
    return {build_market_order_command(account_id, sym, side, shares, dollars, accounts), ""};
}

Validated validate_quick_order_inputs(const std::string& account_id, const std::string& symbol, const std::string& side,
                                      const std::optional<std::string>& dollars_raw, std::optional<double> market_last,
                                      const AccountDirectory& accounts, const QuickMessages& m) {
    if (!accounts.contains(account_id)) return {std::nullopt, m.invalid_account};
    std::string sym = clean_symbol(symbol);
    if (sym.empty()) return {std::nullopt, m.invalid_symbol};
    if (!valid_side(side)) return {std::nullopt, m.invalid_side};
    std::optional<double> dollars;
    try {
        dollars = safe_float(dollars_raw);
    } catch (const std::exception&) {
        return {std::nullopt, m.dollars_positive};
    }
    if (!dollars || *dollars <= 0) return {std::nullopt, m.dollars_positive};
    if (!market_last || !std::isfinite(*market_last) || *market_last <= 0) return {std::nullopt, m.no_last_price};
    double estimate = *dollars / *market_last;
    if (!std::isfinite(estimate)) return {std::nullopt, m.no_last_price};
    long long shares = static_cast<long long>(std::floor(estimate));
    if (shares <= 0) return {std::nullopt, m.dollars_too_low};
    return {build_market_order_command(account_id, sym, side, shares, std::nullopt, accounts), ""};
}

Validated validate_limit_order_inputs(const std::string& account_id, const std::string& symbol, const std::string& side,
                                      const std::optional<std::string>& shares_raw,
                                      const std::optional<std::string>& limit_price_raw,
                                      const std::optional<std::string>& through_market_pct_raw,
                                      std::optional<double> market_last, const AccountDirectory& accounts,
                                      const LimitMessages& m) {
    if (!accounts.contains(account_id)) return {std::nullopt, m.invalid_account};
    std::string sym = clean_symbol(symbol);
    if (sym.empty()) return {std::nullopt, m.invalid_symbol};
    if (!valid_side(side)) return {std::nullopt, m.invalid_side};
    std::optional<long long> shares;
    try {
        shares = safe_int(shares_raw);
    } catch (const std::exception&) {
        return {std::nullopt, m.shares_positive};
    }
    if (!shares || *shares <= 0) return {std::nullopt, m.shares_positive};
    std::optional<double> limit_price;
    try {
        limit_price = safe_float(limit_price_raw);
    } catch (const std::exception&) {
        return {std::nullopt, m.price_positive};
    }
    if (limit_price) {
        if (*limit_price <= 0) return {std::nullopt, m.price_positive};
        return {build_limit_order_command(account_id, sym, side, *shares, *limit_price, accounts), ""};
    }
    std::optional<double> pct;
    try {
        pct = safe_float(through_market_pct_raw);
    } catch (const std::exception&) {
        return {std::nullopt, m.price_positive};
    }
    if (!pct || *pct <= 0) return {std::nullopt, m.price_positive};
    if (!market_last || !std::isfinite(*market_last) || *market_last <= 0) return {std::nullopt, m.no_last_price};
    double price;
    try {
        Decimal change=Decimal::from_double(*pct).scaleb(-2);
        price=(Decimal::from_double(*market_last)*(side=="BUY" ? Decimal(1)+change : Decimal(1)-change)).to_double();
    } catch (const DecimalError&) { return {std::nullopt,m.price_positive}; }
    if (!std::isfinite(price) || price <= 0) return {std::nullopt, m.price_positive};
    // The common publishing boundary applies the instrument's tick once. An
    // intermediate four-decimal rounding could round twice near half a tick.
    return {build_limit_order_command(account_id, sym, side, *shares, price, accounts), ""};
}

Validated validate_cancel_open_orders_inputs(const std::string& account_id, const std::optional<std::string>& symbol,
                                             const AccountDirectory& accounts, const std::string& invalid_account) {
    if (!accounts.contains(account_id)) return {std::nullopt, invalid_account};
    std::string clean = symbol ? clean_symbol(*symbol) : "";
    return {build_cancel_open_orders_command(account_id, accounts, clean.empty() ? std::nullopt : std::optional(clean)), ""};
}

Validated validate_currency_conversion_inputs(const std::string& account_id, const std::optional<std::string>& source,
                                              const std::optional<std::string>& target,
                                              const std::optional<std::string>& amount_raw,
                                              const AccountDirectory& accounts, const FxMessages& m) {
    const AccountMeta* meta = accounts.find(account_id);
    if (!meta) return {std::nullopt, m.invalid_account};
    if (to_lower(trim(meta->broker)) != "tiger") return {std::nullopt, m.invalid_account};
    std::string src = to_upper(trim(source.value_or("")));
    std::string dst = to_upper(trim(target.value_or("")));
    if ((src != "HKD" && src != "USD") || (dst != "HKD" && dst != "USD")) return {std::nullopt, m.invalid_currency};
    if (src == dst) return {std::nullopt, m.same_currency};
    std::optional<double> amount;
    try {
        amount = safe_float(amount_raw);
    } catch (const std::exception&) {
        return {std::nullopt, m.amount_positive};
    }
    if (!amount || *amount <= 0) return {std::nullopt, m.amount_positive};
    return {build_currency_conversion_command(account_id, src, dst, *amount, accounts), ""};
}

Validated validate_trading_status_inputs(const std::string& account_id, const std::string& trading_enabled_raw,
                                         const AccountDirectory& accounts, const TradingStatusMessages& m) {
    std::string clean = trim(account_id);
    if (!accounts.contains(clean)) return {std::nullopt, m.invalid_account};
    std::string raw = to_lower(trim(trading_enabled_raw));
    bool enabled;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on" || raw == "enabled") enabled = true;
    else if (raw == "0" || raw == "false" || raw == "no" || raw == "off" || raw == "disabled") enabled = false;
    else return {std::nullopt, m.invalid_status};
    return {build_trading_status_command(clean, enabled, accounts), ""};
}

namespace {

// parse_fast_trading_config_payload: JSON strings are decoded; dict/other values pass through.
std::variant<json, std::string> decode_fast_config(const json& raw, const std::string& required_message) {
    if (raw.is_null()) return required_message;
    if (raw.is_string()) {
        std::string text = trim(raw.get<std::string>());
        if (text.empty()) return required_message;
        try {
            return json::parse(text);
        } catch (const json::exception&) {
            return required_message;
        }
    }
    return raw;
}

// safe_float(payload.get(key)): float(str(value)) for present values. Booleans stringify to
// "True"/"False" and containers to their repr, neither of which float() accepts.
std::optional<double> value_float(const json* v) {
    if (!v || v->is_null()) return std::nullopt;
    if (v->is_number() && !v->is_boolean()) {
        double d = v->get<double>();
        if (!std::isfinite(d)) throw std::invalid_argument("number must be finite");
        return d;
    }
    if (v->is_string()) return safe_float(v->get<std::string>());
    throw std::invalid_argument("invalid number");
}

std::optional<double> payload_float(const json& payload, const char* key) { return value_float(json_find(payload, key)); }

}  // namespace

std::optional<long long> parse_total_target_position(const json& value) {
    std::optional<double> parsed;
    try {
        parsed = value_float(value.is_null() ? nullptr : &value);
    } catch (const std::exception&) {
        throw std::invalid_argument(kFastTotalTargetPositionError);
    }
    if (!parsed) return std::nullopt;
    if (*parsed < 0 || std::floor(*parsed) != *parsed || *parsed > 9007199254740991.0) {
        throw std::invalid_argument(kFastTotalTargetPositionError);
    }
    return static_cast<long long>(*parsed);
}

std::variant<FastTradingSettings, std::string> parse_fast_trading_settings(const json& raw,
                                                                            const AccountDirectory& accounts,
                                                                            const AlgoMessages& m) {
    auto decoded = decode_fast_config(raw, m.fast_config_required);
    if (auto* err = std::get_if<std::string>(&decoded)) return *err;
    const json& payload = std::get<json>(decoded);
    if (!payload.is_object()) return m.fast_config_required;

    FastTradingSettings settings;
    std::optional<double> price_limit;
    try {
        price_limit = payload_float(payload, "price_limit");
    } catch (const std::exception&) {
        price_limit = std::nullopt;
    }
    if (!price_limit || *price_limit <= 0) return m.fast_price_limit_positive;
    settings.price_limit = *price_limit;

    const json* raw_accounts = json_find(payload, "account_ids");
    if (!raw_accounts || !raw_accounts->is_array() || raw_accounts->empty()) return m.fast_accounts_required;
    std::set<std::string> seen;
    for (const auto& raw_id : *raw_accounts) {
        std::string account_id = trim(json_to_string(&raw_id).value_or(""));
        if (raw_id.is_null()) account_id = "";
        if (!accounts.contains(account_id)) return m.invalid_account;
        if (seen.count(account_id)) return i18n_format_account(m.fast_account_duplicate, account_id);
        seen.insert(account_id);
        settings.account_ids.push_back(account_id);
    }

    struct Choice {
        const char* field;
        std::vector<int> choices;
        const std::string* error;
        int* target;
    };
    std::vector<int> pct_choices(std::begin(kFastTargetPercentageChoices), std::end(kFastTargetPercentageChoices));
    Choice fields[] = {
        {"book_levels", {1, 2, 3, 4, 5}, &m.fast_book_levels_invalid, &settings.book_levels},
        {"target_percentage", pct_choices, &m.fast_target_percentage_invalid, &settings.target_percentage},
        {"cycle_seconds", {5, 10, 15, 30, 60}, &m.fast_cycle_seconds_invalid, &settings.cycle_seconds},
    };
    for (auto& field : fields) {
        std::optional<double> value;
        try {
            value = payload_float(payload, field.field);
        } catch (const std::exception&) {
            value = std::nullopt;
        }
        bool matched = false;
        if (value) {
            for (int choice : field.choices) {
                if (*value == static_cast<double>(choice)) {
                    *field.target = choice;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) return *field.error;
    }
    try {
        const json* target = json_find(payload, "total_target_position");
        settings.total_target_position = parse_total_target_position(target ? *target : json(nullptr));
    } catch (const std::invalid_argument&) {
        return m.fast_total_target_position_invalid;
    }
    settings.test_mode = json_truthy(json_find(payload, "test_mode"));
    return settings;
}

Validated validate_algo_start_inputs(const std::string& trading_mode, const std::string& symbol,
                                     const json& fast_trading_config_raw, const AccountDirectory& accounts,
                                     const AlgoMessages& m) {
    if (trading_mode != "E" && trading_mode != "F") return {std::nullopt, m.invalid_mode};
    std::string sym = clean_symbol(symbol);
    if (sym.empty()) return {std::nullopt, m.invalid_symbol};
    auto parsed = parse_fast_trading_settings(fast_trading_config_raw, accounts, m);
    if (auto* err = std::get_if<std::string>(&parsed)) return {std::nullopt, *err};
    const auto& s = std::get<FastTradingSettings>(parsed);
    return {build_algo_start_command(trading_mode, sym, s.price_limit, s.account_ids, s.book_levels, s.target_percentage,
                                     s.cycle_seconds, s.test_mode, s.total_target_position),
            ""};
}

json extract_fast_trading_config(const json& body) {
    const json* raw_config = json_find(body, "fast_trading_config");
    if (raw_config && !raw_config->is_null() && !raw_config->is_object()) return *raw_config;
    json config = (raw_config && raw_config->is_object()) ? *raw_config : json::object();
    static const std::vector<std::pair<const char*, std::vector<const char*>>> aliases = {
        {"price_limit", {"fast_trading_price_limit", "price_limit"}},
        {"account_ids", {"fast_trading_account_ids", "account_ids"}},
        {"book_levels", {"fast_trading_book_levels", "book_levels"}},
        {"target_percentage", {"fast_trading_target_percentage", "target_percentage"}},
        {"cycle_seconds", {"fast_trading_cycle_seconds", "cycle_seconds"}},
        {"total_target_position", {"fast_trading_total_target_position", "total_target_position"}},
        {"test_mode", {"fast_trading_test_mode", "test_mode"}},
    };
    bool found_alias = false;
    for (auto& [key, candidates] : aliases) {
        if (config.contains(key)) continue;
        for (const char* candidate : candidates) {
            if (const json* v = json_find(body, candidate)) {
                config[key] = *v;
                found_alias = true;
                break;
            }
        }
    }
    if ((!raw_config || raw_config->is_null()) && !found_alias) return json(nullptr);
    return config;
}

}  // namespace ng::orders

namespace ng::orders {
std::string i18n_format_account(const std::string& text, const std::string& account) {
    std::string out = text;
    std::size_t pos = out.find("{account}");
    if (pos != std::string::npos) out.replace(pos, 9, account);
    return out;
}
}  // namespace ng::orders
