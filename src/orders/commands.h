// Kafka trading-command builders and input validation (trading_ui/services/orders.py).
// Every validator returns either a command object or a localized error message.
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/json.h"
#include "model/account.h"

namespace ng::orders {

struct Validated {
    std::optional<json> command;
    std::string error;  // localized; empty when command is set
    bool ok() const { return command.has_value(); }
};

struct OrderMessages {  // localized strings injected by the caller
    std::string invalid_account, invalid_symbol, invalid_side, both_shares_and_dollars, neither_shares_nor_dollars,
        shares_positive, dollars_positive;
};

struct QuickMessages {
    std::string invalid_account, invalid_symbol, invalid_side, dollars_positive, no_last_price, dollars_too_low;
};

struct LimitMessages {
    std::string invalid_account, invalid_symbol, invalid_side, shares_positive, price_positive, no_last_price;
};

struct FxMessages {
    std::string invalid_account, invalid_currency, same_currency, amount_positive;
};

struct TradingStatusMessages {
    std::string invalid_account, invalid_status;
};

inline constexpr const char* kFastTotalTargetPositionError = "Enter a whole number of shares (0 or more), or leave blank.";
inline constexpr const char* kBrokerOrdersTimezone = "America/New_York";

struct AlgoMessages {
    std::string invalid_mode, invalid_symbol, invalid_account, fast_config_required, fast_price_limit_positive,
        fast_accounts_required, fast_account_duplicate, fast_book_levels_invalid, fast_target_percentage_invalid,
        fast_cycle_seconds_invalid;
    std::string fast_total_target_position_invalid = kFastTotalTargetPositionError;
};

struct BrokerOrdersMessages {
    std::string invalid_date, future_date, invalid_account, accounts_required;
};

std::string iso_utc_now();
std::string i18n_format_account(const std::string& text, const std::string& account);
std::string new_command_id(const std::string& prefix);
// Round positive limit prices to the nearest configured tick; halfway rounds up.
double nearest_tick_price(const json& price, const std::string& tick_size);

// Pure builders.
json build_market_order_command(const std::string& account_id, const std::string& symbol, const std::string& side,
                                std::optional<long long> shares, std::optional<double> dollar_amount,
                                const AccountDirectory& accounts);
json build_limit_order_command(const std::string& account_id, const std::string& symbol, const std::string& side,
                               long long shares, double limit_price, const AccountDirectory& accounts);
json build_limit_order_fok_command(const std::string& account_id, const std::string& symbol, const std::string& side,
                                   long long shares, double limit_price, const AccountDirectory& accounts);
json build_cancel_open_orders_command(const std::string& account_id, const AccountDirectory& accounts,
                                      const std::optional<std::string>& symbol = std::nullopt);
json build_currency_conversion_command(const std::string& account_id, const std::string& source_currency,
                                       const std::string& target_currency, double source_amount,
                                       const AccountDirectory& accounts);
json build_trading_status_command(const std::string& account_id, bool trading_enabled, const AccountDirectory& accounts);
json build_algo_start_command(const std::string& trading_mode, const std::string& symbol, double price_limit,
                              const std::vector<std::string>& account_ids, int book_levels, int target_percentage,
                              int cycle_seconds, bool test_mode,
                              std::optional<long long> total_target_position = std::nullopt);
json build_algo_stop_command(const std::optional<std::string>& reason);
// Optional fast-trading position target: nullopt when absent/blank, else a whole number of shares in
// [0, 2^53-1]. Throws std::invalid_argument(kFastTotalTargetPositionError) for anything else.
std::optional<long long> parse_total_target_position(const json& value);
// Today's calendar date ("YYYY-MM-DD") in America/New_York.
std::string broker_orders_today();
// One read-only, multi-account GET_BROKER_ORDERS request; the UI-only "__ALL__" selection expands to
// every configured account. `today` overrides the New York date in tests.
Validated validate_broker_orders_inputs(const std::string& trading_date_raw, const std::vector<std::string>& account_ids,
                                        const AccountDirectory& accounts, const BrokerOrdersMessages& m,
                                        const std::optional<std::string>& today = std::nullopt);

// Validators (raw form/JSON text inputs). `*_raw` strings may be empty/absent.
Validated validate_order_inputs(const std::string& account_id, const std::string& symbol, const std::string& side,
                                const std::optional<std::string>& shares_raw, const std::optional<std::string>& dollars_raw,
                                const AccountDirectory& accounts, const OrderMessages& m);
Validated validate_quick_order_inputs(const std::string& account_id, const std::string& symbol, const std::string& side,
                                      const std::optional<std::string>& dollars_raw, std::optional<double> market_last,
                                      const AccountDirectory& accounts, const QuickMessages& m);
Validated validate_limit_order_inputs(const std::string& account_id, const std::string& symbol, const std::string& side,
                                      const std::optional<std::string>& shares_raw,
                                      const std::optional<std::string>& limit_price_raw,
                                      const std::optional<std::string>& through_market_pct_raw,
                                      std::optional<double> market_last, const AccountDirectory& accounts,
                                      const LimitMessages& m);
Validated validate_cancel_open_orders_inputs(const std::string& account_id, const std::optional<std::string>& symbol,
                                             const AccountDirectory& accounts, const std::string& invalid_account);
Validated validate_currency_conversion_inputs(const std::string& account_id, const std::optional<std::string>& source,
                                              const std::optional<std::string>& target,
                                              const std::optional<std::string>& amount_raw,
                                              const AccountDirectory& accounts, const FxMessages& m);
Validated validate_trading_status_inputs(const std::string& account_id, const std::string& trading_enabled_raw,
                                         const AccountDirectory& accounts, const TradingStatusMessages& m);

struct FastTradingSettings {
    double price_limit = 0.0;
    std::vector<std::string> account_ids;
    int book_levels = 0;
    int target_percentage = 0;
    int cycle_seconds = 0;
    bool test_mode = false;
    std::optional<long long> total_target_position;
};

// `fast_trading_config_raw` is a JSON string (from a form) or a JSON object/other value (from the API);
// null means absent.
std::variant<FastTradingSettings, std::string> parse_fast_trading_settings(const json& fast_trading_config_raw,
                                                                            const AccountDirectory& accounts,
                                                                            const AlgoMessages& m);
Validated validate_algo_start_inputs(const std::string& trading_mode, const std::string& symbol,
                                     const json& fast_trading_config_raw, const AccountDirectory& accounts,
                                     const AlgoMessages& m);
// Merges top-level fast_trading_* aliases into the nested config (webserver._extract_fast_trading_config).
json extract_fast_trading_config(const json& body);

// Parsing helpers shared with the web layer. Throw std::invalid_argument on malformed input.
std::optional<double> safe_float(const std::optional<std::string>& raw);
std::optional<long long> safe_int(const std::optional<std::string>& raw);

inline constexpr int kFastBookLevelChoices[] = {1, 2, 3, 4, 5};
inline constexpr int kFastTargetPercentageChoices[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 120, 150};
inline constexpr int kFastCycleSecondsChoices[] = {5, 10, 15, 30, 60};

}  // namespace ng::orders
