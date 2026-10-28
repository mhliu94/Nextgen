// Account metadata (from trading_accounts.json) and live account snapshots (from Kafka).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace ng {

struct AccountMeta {
    std::string id;
    int num_id = 0;
    std::string broker;
    std::string trading_medium = "API";
    std::string broker_id;
    std::optional<std::string> ip_address;
    std::optional<std::string> machine_alias;
    bool monitor = false;

    std::string label() const;         // "#<num> <id>"
    std::string server_title() const;  // "<id> | <broker> | <alias> | <ip>"
};

struct Position {
    std::string symbol;
    double qty = 0.0;
    std::optional<double> avg_price;
    std::optional<double> available_qty;
    json to_json() const;
};

struct AccountSnapshot {
    std::string account_id;
    double cash = 0.0;
    std::optional<int> account_num_id;
    std::map<std::string, double> cash_by_currency;
    std::map<std::string, double> available_cash_by_currency;
    std::vector<Position> positions;
    std::optional<std::string> ts;
    bool trading_enabled = false;

    json to_json() const;
    // USD cash the way the dashboard and E-mode capacity compute it:
    // cash_by_currency["USD"] when present, else `cash`.
    double usd_cash() const;
    // Available USD cash: available_cash_by_currency["USD"], then cash_by_currency["USD"], then cash.
    double available_usd_cash() const;
    // Net quantity held for a (canonicalised) symbol.
    double symbol_qty(const std::string& canonical) const;
    // Snapshot timestamp as epoch seconds (nullopt if unparseable).
    std::optional<double> ts_epoch() const;
};

// Parses one account-details Kafka payload (parse_snapshot_obj). Returns nullopt for malformed input.
std::optional<AccountSnapshot> parse_snapshot_obj(const json& payload);

// Parses a trading account entry list (parse_account_metas); hidden accounts are skipped unless requested.
// Throws std::invalid_argument with the Python server's messages.
std::vector<AccountMeta> parse_account_metas(const json& accounts, bool require_static_fields, bool include_hidden);

// Ordered account directory with id lookup.
class AccountDirectory {
public:
    AccountDirectory() = default;
    explicit AccountDirectory(std::vector<AccountMeta> metas);
    const std::vector<AccountMeta>& ordered() const { return ordered_; }
    // Sorted by numeric id ascending.
    std::vector<const AccountMeta*> by_num_id() const;
    const AccountMeta* find(const std::string& id) const;
    const AccountMeta* find_by_num(int num_id) const;
    bool contains(const std::string& id) const { return find(id) != nullptr; }
    std::size_t size() const { return ordered_.size(); }
    bool empty() const { return ordered_.empty(); }
    std::vector<std::string> ids_by_num_id() const;

private:
    std::vector<AccountMeta> ordered_;
    std::map<std::string, std::size_t> index_;
    std::map<int, std::size_t> num_index_;
};

}  // namespace ng
