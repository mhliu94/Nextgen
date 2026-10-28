#include "model/account.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng {

std::string AccountMeta::label() const { return "#" + std::to_string(num_id) + " " + id; }

std::string AccountMeta::server_title() const {
    std::vector<std::string> parts{id, broker};
    if (machine_alias && !machine_alias->empty()) parts.push_back(*machine_alias);
    if (ip_address && !ip_address->empty()) parts.push_back(*ip_address);
    std::vector<std::string> kept;
    for (auto& p : parts) if (!p.empty()) kept.push_back(p);
    return join(kept, " | ");
}

json Position::to_json() const {
    json out;
    out["symbol"] = symbol;
    out["qty"] = qty;
    if (avg_price) out["avg_price"] = *avg_price; else out["avg_price"] = nullptr;
    if (available_qty) out["available_qty"] = *available_qty; else out["available_qty"] = nullptr;
    return out;
}

json AccountSnapshot::to_json() const {
    json out;
    out["account_id"] = account_id;
    if (account_num_id) out["account_num_id"] = *account_num_id; else out["account_num_id"] = nullptr;
    out["cash"] = cash;
    out["cash_by_currency"] = json::object();
    for (auto& [k, v] : cash_by_currency) out["cash_by_currency"][k] = v;
    out["available_cash_by_currency"] = json::object();
    for (auto& [k, v] : available_cash_by_currency) out["available_cash_by_currency"][k] = v;
    if (ts) out["ts"] = *ts; else out["ts"] = nullptr;
    out["trading_enabled"] = trading_enabled;
    out["positions"] = json::array();
    for (auto& p : positions) out["positions"].push_back(p.to_json());
    return out;
}

double AccountSnapshot::usd_cash() const {
    for (auto& [currency, amount] : cash_by_currency) {
        if (to_upper(trim(currency)) == "USD") return amount;
    }
    return cash;
}

double AccountSnapshot::available_usd_cash() const {
    auto it = available_cash_by_currency.find("USD");
    if (it != available_cash_by_currency.end() && std::isfinite(it->second)) return it->second;
    auto cit = cash_by_currency.find("USD");
    if (cit != cash_by_currency.end() && std::isfinite(cit->second)) return cit->second;
    return std::isfinite(cash) ? cash : 0.0;
}

double AccountSnapshot::symbol_qty(const std::string& canonical) const {
    double total = 0.0;
    for (auto& p : positions) {
        if (canonical_symbol(p.symbol) == canonical) total += p.qty;
    }
    return total;
}

std::optional<double> AccountSnapshot::ts_epoch() const {
    if (!ts) return std::nullopt;
    auto parsed = parse_iso8601(*ts);
    if (!parsed) return std::nullopt;
    return parsed->epoch_seconds;
}

std::optional<AccountSnapshot> parse_snapshot_obj(const json& obj) {
    if (!obj.is_object()) return std::nullopt;
    AccountSnapshot snap;
    const json* id = json_find(obj, "account_id");
    if (!id) return std::nullopt;
    auto id_text = json_to_string(id);
    if (!id_text) return std::nullopt;  // str(None) would be "None" in Python; treat null as invalid
    snap.account_id = trim(*id_text);
    if (snap.account_id.empty()) return std::nullopt;

    if (const json* num = json_find(obj, "account_num_id")) {
        bool blank = num->is_null() || (num->is_string() && num->get<std::string>().empty());
        if (!blank) {
            auto parsed = json_to_int(num);
            if (!parsed) return std::nullopt;
            snap.account_num_id = static_cast<int>(*parsed);
        }
    }

    if (const json* cash = json_find(obj, "cash")) {
        auto parsed = json_to_double(cash);
        if (!parsed) return std::nullopt;
        snap.cash = *parsed;
    } else {
        snap.cash = 0.0;
    }

    if (const json* raw = json_find(obj, "cash_by_currency"); raw && raw->is_object()) {
        for (auto it = raw->begin(); it != raw->end(); ++it) {
            std::string currency = to_upper(trim(it.key()));
            if (currency.empty()) continue;
            if (it.value().is_null()) {
                snap.cash_by_currency[currency] = 0.0;
                continue;
            }
            auto amount = json_to_double(&it.value());
            if (!amount) return std::nullopt;  // float(garbage) raises in Python
            snap.cash_by_currency[currency] = *amount;
        }
    }
    if (!snap.cash_by_currency.count("USD")) snap.cash_by_currency["USD"] = snap.cash;

    if (const json* raw = json_find(obj, "available_cash_by_currency"); raw && raw->is_object()) {
        for (auto it = raw->begin(); it != raw->end(); ++it) {
            std::string currency = to_upper(trim(it.key()));
            if (currency.empty()) continue;
            auto amount = json_finite_double(&it.value());
            if (amount) snap.available_cash_by_currency[currency] = *amount;
        }
    }

    if (const json* ts = json_find(obj, "ts"); ts && !ts->is_null()) snap.ts = json_to_string(ts);
    snap.trading_enabled = json_parse_bool(json_find(obj, "trading_enabled"));

    if (const json* raw = json_find(obj, "positions"); raw && !raw->is_null()) {
        if (!raw->is_array()) return std::nullopt;
        for (const auto& p : *raw) {
            if (!p.is_object()) return std::nullopt;
            const json* symbol = json_find(p, "symbol");
            if (!symbol) return std::nullopt;
            Position position;
            position.symbol = json_to_string(symbol).value_or("None");
            if (const json* qty = json_find(p, "qty"); qty && !qty->is_null()) {
                auto parsed = json_to_double(qty);
                if (!parsed) return std::nullopt;
                position.qty = *parsed;
            } else if (qty && qty->is_null()) {
                return std::nullopt;  // float(None)
            } else {
                position.qty = 0.0;
            }
            if (const json* avg = json_find(p, "avg_price"); avg && !avg->is_null()) {
                auto parsed = json_to_double(avg);
                if (!parsed) return std::nullopt;
                position.avg_price = *parsed;
            }
            const json* avail = json_find(p, "available_qty");
            if (!avail || avail->is_null()) avail = json_find(p, "salable_qty");
            position.available_qty = json_finite_double(avail);
            snap.positions.push_back(std::move(position));
        }
    }
    return snap;
}

namespace {

const json* account_field(const json& account, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (const json* v = json_find(account, name)) return v;
    }
    return nullptr;
}

std::string required_account_string(const json& account, std::initializer_list<const char*> names, const char* label) {
    const json* v = account_field(account, names);
    if (!v || !v->is_string() || trim(v->get<std::string>()).empty()) {
        throw std::invalid_argument(std::string("Account entry missing required string field '") + label + "'.");
    }
    return trim(v->get<std::string>());
}

std::optional<std::string> optional_account_string(const json& account, std::initializer_list<const char*> names,
                                                   const char* label) {
    const json* v = account_field(account, names);
    if (!v || v->is_null() || (v->is_string() && v->get<std::string>().empty())) return std::nullopt;
    if (!v->is_string()) {
        throw std::invalid_argument(std::string("Account field '") + label + "' must be a string when provided.");
    }
    std::string value = trim(v->get<std::string>());
    if (value.empty()) return std::nullopt;
    return value;
}

}  // namespace

std::vector<AccountMeta> parse_account_metas(const json& accounts, bool require_static_fields, bool include_hidden) {
    if (!accounts.is_array() || accounts.empty()) {
        throw std::invalid_argument("Trading accounts must contain a non-empty accounts list.");
    }
    static const std::set<std::string> media = {"EMULATOR", "WINDOWS", "WEB", "API"};
    std::vector<AccountMeta> metas;
    std::set<int> seen_num;
    std::set<std::string> seen_ids;
    int idx = 0;
    for (const auto& a : accounts) {
        ++idx;
        if (!a.is_object()) throw std::invalid_argument("Account entry #" + std::to_string(idx) + " must be an object.");
        if (!include_hidden) {
            const json* hidden = account_field(a, {"hidden", "Hidden"});
            if (hidden && hidden->is_boolean() && hidden->get<bool>()) continue;
        }
        const json* raw_id = account_field(a, {"string_id", "id", "account_id"});
        if (!raw_id || !raw_id->is_string() || trim(raw_id->get<std::string>()).empty()) {
            throw std::invalid_argument("Account entry #" + std::to_string(idx) + " missing required string_id.");
        }
        std::string string_id = trim(raw_id->get<std::string>());
        if (seen_ids.count(string_id)) throw std::invalid_argument("Duplicate string_id in accounts config: " + string_id);
        seen_ids.insert(string_id);

        const json* raw_num = account_field(a, {"numeric_id", "num_id", "account_num_id"});
        auto num = json_to_int(raw_num);
        if (!num || (raw_num && raw_num->is_boolean())) {
            std::string shown = raw_num ? raw_num->dump() : "None";
            throw std::invalid_argument("Invalid numeric_id for account '" + string_id + "': " + shown);
        }
        if (*num < 1) {
            throw std::invalid_argument("numeric_id must be positive for account '" + string_id + "': " + std::to_string(*num));
        }
        if (seen_num.count(static_cast<int>(*num))) {
            throw std::invalid_argument("Duplicate numeric_id in accounts config: " + std::to_string(*num));
        }
        seen_num.insert(static_cast<int>(*num));

        std::string broker = required_account_string(a, {"broker"}, "broker");
        const json* raw_medium = account_field(a, {"trading_medium", "medium"});
        std::string medium_text;
        bool medium_blank = !raw_medium || raw_medium->is_null() || (raw_medium->is_string() && raw_medium->get<std::string>().empty());
        if (medium_blank && !require_static_fields) {
            medium_text = "API";
        } else {
            if (!raw_medium || !raw_medium->is_string() || trim(raw_medium->get<std::string>()).empty()) {
                throw std::invalid_argument("Account '" + string_id + "' missing required trading_medium.");
            }
            medium_text = raw_medium->get<std::string>();
        }
        std::string medium = to_upper(trim(medium_text));
        if (!media.count(medium)) {
            throw std::invalid_argument("Invalid trading_medium for account '" + string_id + "': '" + medium_text +
                                        "'. Allowed: API, EMULATOR, WEB, WINDOWS");
        }
        AccountMeta meta;
        meta.id = string_id;
        meta.num_id = static_cast<int>(*num);
        meta.broker = broker;
        meta.trading_medium = medium;
        if (const json* bid = json_find(a, "broker_id"); bid && !bid->is_null()) {
            meta.broker_id = trim(json_to_string(bid).value_or(""));
        }
        meta.ip_address = optional_account_string(a, {"ip_address", "ip"}, "ip_address");
        meta.machine_alias = optional_account_string(a, {"machine_alias"}, "machine_alias");
        const json* monitor = json_find(a, "monitor");
        meta.monitor = monitor && monitor->is_boolean() && monitor->get<bool>();
        metas.push_back(std::move(meta));
    }
    return metas;
}

AccountDirectory::AccountDirectory(std::vector<AccountMeta> metas) : ordered_(std::move(metas)) {
    for (std::size_t i = 0; i < ordered_.size(); ++i) {
        index_[ordered_[i].id] = i;
        num_index_[ordered_[i].num_id] = i;
    }
}

std::vector<const AccountMeta*> AccountDirectory::by_num_id() const {
    std::vector<const AccountMeta*> out;
    out.reserve(ordered_.size());
    for (auto& m : ordered_) out.push_back(&m);
    std::sort(out.begin(), out.end(), [](const AccountMeta* a, const AccountMeta* b) {
        if (a->num_id != b->num_id) return a->num_id < b->num_id;
        return a->id < b->id;
    });
    return out;
}

std::vector<std::string> AccountDirectory::ids_by_num_id() const {
    std::vector<std::string> out;
    for (auto* m : by_num_id()) out.push_back(m->id);
    return out;
}

const AccountMeta* AccountDirectory::find(const std::string& id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : &ordered_[it->second];
}

const AccountMeta* AccountDirectory::find_by_num(int num_id) const {
    auto it = num_index_.find(num_id);
    return it == num_index_.end() ? nullptr : &ordered_[it->second];
}

}  // namespace ng
