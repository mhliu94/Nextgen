#include "model/account_delta.h"
#include <cmath>
#include <map>
#include <stdexcept>
#include "core/strings.h"
#include "core/time_util.h"

namespace ng {
namespace {
std::string text(const json& value) {
    if (!value.is_string()) throw std::invalid_argument("Expected account text");
    auto s = value.get<std::string>();
    if (s.empty() || s.size() > 256 || s != trim(s)) throw std::invalid_argument("Invalid account text");
    return s;
}
double amount(const json& value) {
    if (value.is_boolean()) throw std::invalid_argument("Boolean account amount");
    auto n = json_finite_double(&value);
    if (!n) throw std::invalid_argument("Invalid account amount");
    return *n;
}
std::int64_t sequence(const json& value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX))
        throw std::invalid_argument("Invalid account sequence");
    auto n = value.get<std::int64_t>();
    if (n < 1) throw std::invalid_argument("Invalid account sequence");
    return n;
}
double timestamp(const json& value) {
    auto stamp = parse_iso8601(text(value));
    if (!stamp || !stamp->has_offset) throw std::invalid_argument("Invalid account timestamp");
    return stamp->epoch_seconds;
}
std::map<std::string, json> positions(const json& rows) {
    if (!rows.is_array()) throw std::invalid_argument("Invalid account positions");
    std::map<std::string, json> out;
    for (const auto& row : rows) {
        auto symbol = text(row.at("symbol"));
        json normalized = {{"symbol",symbol},{"qty",amount(row.at("qty"))},{"avg_price",nullptr}};
        if (row.contains("avg_price") && !row.at("avg_price").is_null()) normalized["avg_price"] = amount(row.at("avg_price"));
        const json* available = json_find(row,"available_qty");
        if (!available || available->is_null()) available = json_find(row,"salable_qty");
        if (available && !available->is_null()) normalized["available_qty"] = amount(*available);
        if (!out.emplace(symbol,std::move(normalized)).second) throw std::invalid_argument("Duplicate account position");
    }
    return out;
}
json normalize(const json& value) {
    json out = {{"account_id",text(value.at("account_id"))},{"cash",amount(value.at("cash"))},
                {"account_num_id",value.value("account_num_id",json(nullptr))},{"ts",value.at("ts")},
                {"trading_enabled",value.at("trading_enabled")}};
    timestamp(out.at("ts"));
    if (!out.at("trading_enabled").is_boolean()) throw std::invalid_argument("Invalid trading state");
    if (!out.at("account_num_id").is_null()) sequence(out.at("account_num_id"));
    for (const auto* field : {"cash_by_currency","available_cash_by_currency"}) {
        auto values = value.value(field,json::object());
        if (!values.is_object()) throw std::invalid_argument("Invalid cash map");
        out[field] = json::object();
        for (auto it=values.begin();it!=values.end();++it) out[field][text(it.key())] = amount(it.value());
    }
    if (!out["cash_by_currency"].contains("USD")) out["cash_by_currency"]["USD"] = out["cash"];
    if (out["cash_by_currency"]["USD"] != out["cash"]) throw std::invalid_argument("Inconsistent USD cash");
    if (value.contains("cash_basis") && !value["cash_basis"].is_null()) out["cash_basis"] = text(value["cash_basis"]);
    if (value.contains("buying_power") && !value["buying_power"].is_null()) out["buying_power"] = amount(value["buying_power"]);
    out["positions"] = json::array();
    for (const auto& [symbol,row] : positions(value.at("positions"))) out["positions"].push_back(row);
    return out;
}
}

std::optional<json> AccountReplica::apply(const json& event) {
    if (!event.is_object() || !event.contains("account_update_version") ||
        !event["account_update_version"].is_number_integer() || event["account_update_version"] != 1)
        throw std::invalid_argument("Unsupported account update version");
    auto stream = text(event.at("stream_id"));
    auto seq = sequence(event.at("sequence"));
    auto kind = text(event.at("message_type"));
    if (kind != "account_snapshot" && kind != "account_delta") throw std::invalid_argument("Unknown account message type");
    auto stamp = timestamp(event.at("ts"));
    if (retired_.count(stream) || (stream==stream_ && seq<=sequence_)) return std::nullopt;
    if (state_) {
        if (event.at("account_id") != state_->at("account_id") ||
            event.value("account_num_id",json(nullptr)) != state_->at("account_num_id"))
            throw std::invalid_argument("Account identity mismatch");
        if (stamp < timestamp(state_->at("ts"))) return std::nullopt;
    }
    json next;
    if (kind == "account_snapshot") next = normalize(event);
    else {
        if (missing_ || !state_ || stream!=stream_ || !event.contains("base_sequence") ||
            sequence(event.at("base_sequence"))!=sequence_ || sequence_==INT64_MAX || seq!=sequence_+1) {
            missing_=true;
            return std::nullopt;
        }
        next=*state_;
        auto rows=positions(next.at("positions"));
        auto changed=positions(event.at("positions"));
        const auto& removed=event.at("removed_positions");
        if (!removed.is_array()) throw std::invalid_argument("Invalid position removals");
        std::set<std::string> removals;
        for (const auto& v:removed) {
            auto symbol=text(v);
            if (!removals.insert(symbol).second || changed.count(symbol)) throw std::invalid_argument("Conflicting position operations");
            rows.erase(symbol);
        }
        for (const auto& [symbol,row]:changed) rows[symbol]=row;
        next["positions"]=json::array();
        for (const auto& [symbol,row]:rows) next["positions"].push_back(row);
        for (const auto* field:{"cash_by_currency","available_cash_by_currency"}) {
            auto changes=event.value(field,json::object());
            if (!changes.is_object()) throw std::invalid_argument("Invalid cash changes");
            for (auto it=changes.begin();it!=changes.end();++it) {
                auto currency=text(it.key());
                if (it.value().is_null()) next[field].erase(currency);
                else next[field][currency]=amount(it.value());
            }
        }
        for (const auto* field:{"cash","trading_enabled","cash_basis","buying_power"})
            if (event.contains(field)) next[field]=event.at(field);
        next["ts"]=event.at("ts");
        next=normalize(next);
    }
    if (!stream_.empty() && stream!=stream_) retired_.insert(stream_);
    state_=next; stream_=stream; sequence_=seq; missing_=false;
    return next;
}
}
