#include "order_ui/reconciliation.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <tuple>

#include "core/sqlite.h"
#include "core/strings.h"
#include "core/time_util.h"

namespace ng::order_ui {
namespace {

bool is_terminal(const std::string& status) {
    return status == "FILLED" || status == "CANCELED" || status == "REJECTED" || status == "EXPIRED";
}

// A consistent read-only snapshot of one store; SQLite never creates or modifies the file.
struct ReadOnlyDatabase {
    Sqlite db;
    explicit ReadOnlyDatabase(const std::string& path) : db(path, Sqlite::Mode::ReadOnly, 500) {
        db.exec("PRAGMA query_only=ON");
        db.begin();
    }
};

json command_time(const std::string& command_id) {
    static const std::regex pattern(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z)");
    std::smatch match;
    if (!std::regex_search(command_id, match, pattern)) return nullptr;
    return match.str(0);
}

const char* reason_for(const std::string& status) {
    if (status == "PENDING_NEW") return "awaiting_acceptance";
    if (status == "PENDING_CANCEL") return "awaiting_cancellation";
    if (status == "UNKNOWN") return "status_unknown";
    return "working_order";
}

std::vector<json> feedback_items(Sqlite& db) {
    std::vector<json> items;
    {
        Statement rows(db, "SELECT * FROM feedback_orders WHERE status NOT IN ('FILLED','CANCELED','REJECTED','EXPIRED')");
        while (rows.step()) {
            json row = rows.row_json();
            row["kind"] = "broker_order";
            row["reason"] = reason_for(row.at("status").get<std::string>());
            items.push_back(std::move(row));
        }
    }
    {
        Statement rows(db, "SELECT * FROM feedback_pending_commands WHERE pending=1");
        while (rows.step()) {
            json row = rows.row_json();
            json stamp = command_time(row.at("command_id").get<std::string>());
            row["kind"] = "pending_command";
            row["status"] = "AWAITING_REPORT";
            row["reason"] = "missing_feedback";
            row["created_at"] = stamp;
            row["updated_at"] = stamp;
            items.push_back(std::move(row));
        }
    }
    return items;
}

std::vector<json> child_items(Sqlite& db) {
    std::vector<json> items;
    Statement rows(db, "SELECT id,data FROM smart_parents WHERE active=1");
    while (rows.step()) {
        json parent = json::parse(rows.column_text(1));
        const json* order = json_find(parent, "order");
        if (!order || !order->is_object() || !json_python_truthy(json_find(*order, "dispatch_prepared")) ||
            order->at("state") == "TERMINAL") {
            continue;
        }
        std::string sent = iso_utc_offset(order->at("send_wall").get<double>());
        json item;
        item["kind"] = "algo_order";
        item["account_id"] = order->at("account_id");
        item["symbol"] = parent.at("symbol");
        item["side"] = parent.contains("side") ? parent.at("side") : json(nullptr);
        item["order_id"] = order->contains("broker_order_id") ? order->at("broker_order_id") : json(nullptr);
        item["command_id"] = order->at("client_order_id");
        item["session_id"] = rows.column_text(0);
        item["session_state"] = parent.at("state");
        item["status"] = order->at("state");
        item["quantity"] = order->at("quantity");
        item["filled_quantity"] = order->at("cumulative_filled");
        item["limit_price"] = order->at("limit_price");
        item["created_at"] = sent;
        item["updated_at"] = sent;
        item["reason"] = order->at("state") == "RECONCILE" ? "algo_reconciliation" : "awaiting_terminal";
        items.push_back(std::move(item));
    }
    return items;
}

// Python's `value or other`: null, "", 0 and false fall through.
const json& first_truthy(const json& a, const json& b) { return json_python_truthy(&a) ? a : b; }

json get_or_null(const json& object, const char* key) { return object.contains(key) ? object.at(key) : json(nullptr); }

}  // namespace

ReconciliationView::ReconciliationView(std::vector<ReconciliationSource> sources, std::string reporting_db_path, Clock now)
    : sources_(std::move(sources)), reporting_db_path_(std::move(reporting_db_path)), now_(now ? std::move(now) : Clock(&wall_now)) {}

json ReconciliationView::snapshot(const AccountDirectory& accounts) const {
    // Rows keyed by (account, "order" | "command", id), in first-seen order.
    using Key = std::tuple<std::string, std::string, std::string>;
    std::vector<json> rows;
    std::map<Key, std::size_t> index;

    auto merge = [&](json item, const std::string& mode) {
        std::string account = item.at("account_id").get<std::string>();
        if (!accounts.contains(account)) return;
        json order_id = get_or_null(item, "order_id");
        bool has_order = json_python_truthy(&order_id);
        const json& identity = has_order ? order_id : item.at("command_id");
        Key key{account, has_order ? "order" : "command", identity.is_string() ? identity.get<std::string>() : identity.dump()};
        auto found = index.find(key);
        if (found == index.end()) {
            item["modes"] = json::array({mode});
            index.emplace(std::move(key), rows.size());
            rows.push_back(std::move(item));
            return;
        }
        json& row = rows[found->second];
        // Preserve blockers from either controller, even if their copies disagree; never silently
        // hide a still-persisted reservation.
        std::string kind = item.value("kind", std::string());
        if (kind == "algo_order") {
            for (auto it = item.begin(); it != item.end(); ++it) {
                if (!it.value().is_null() && it.key() != "created_at" && it.key() != "updated_at") row[it.key()] = it.value();
            }
        } else if (kind == "broker_order" && row.value("kind", std::string()) == "broker_order" &&
                   item.at("sequence").get<long long>() > row.at("sequence").get<long long>()) {
            row.update(item);
        }
        std::set<std::string> modes;
        for (const auto& existing : row.at("modes")) modes.insert(existing.get<std::string>());
        modes.insert(mode);
        row["modes"] = modes;
    };

    json health_reports = json::array();
    for (const ReconciliationSource& source : sources_) {
        json health = {{"mode", source.mode}};
        for (const char* part : {"feedback", "state"}) {
            std::string path = std::string(part) == "feedback" ? source.state_db + ".feedback.sqlite3" : source.state_db;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) {
                health[part] = "missing";
                continue;
            }
            try {
                std::vector<json> items;
                {
                    ReadOnlyDatabase database(path);
                    items = std::string(part) == "feedback" ? feedback_items(database.db) : child_items(database.db);
                }
                for (json& item : items) {
                    if (item.at("kind") == "algo_order") item["side"] = source.mode == "G" ? "SELL" : "BUY";
                    merge(std::move(item), source.mode);
                }
                health[part] = "available";
            } catch (const std::exception&) {
                health[part] = "unavailable";
            }
        }
        health_reports.push_back(std::move(health));
    }

    bool details_available = true;
    try {
        ReadOnlyDatabase database(reporting_db_path_);
        for (json& row : rows) {
            json order_id = get_or_null(row, "order_id");
            if (!json_python_truthy(&order_id)) continue;
            Statement found(database.db, "SELECT payload FROM activity_orders WHERE account_id=? AND order_id=?");
            found.bind(1, row.at("account_id").get<std::string>()).bind(2, order_id.get<std::string>());
            if (!found.step()) continue;
            json detail = json::parse(found.column_text(0));
            if (!detail.is_object()) throw std::invalid_argument("order payload must be an object");
            for (const char* field : {"quantity", "filled_quantity", "limit_price", "message", "order_type"}) {
                row[field] = get_or_null(detail, field);
            }
            json command_id = get_or_null(row, "command_id");
            json detail_command = get_or_null(detail, "command_id");
            json detail_client = get_or_null(detail, "client_order_id");
            row["command_id"] = json_python_truthy(&command_id) ? command_id : first_truthy(detail_command, detail_client);
            json reported = get_or_null(detail, "status");
            row["reported_status"] = reported;
            if (reported.is_string() && is_terminal(reported.get<std::string>())) row["reason"] = "controller_pending";
        }
    } catch (const std::exception&) {
        details_available = false;
    }

    double now = now_();
    for (json& row : rows) {
        const AccountMeta* meta = accounts.find(row.at("account_id").get<std::string>());
        row["account_num_id"] = meta->num_id;
        row["broker"] = meta->broker;
        row["age_seconds"] = nullptr;
        json updated = get_or_null(row, "updated_at");
        if (updated.is_string()) {
            auto stamp = parse_iso8601(updated.get<std::string>());
            if (stamp && stamp->has_offset) {
                long long micros = std::llround(now * 1e6) - std::llround(stamp->epoch_seconds * 1e6);
                row["age_seconds"] = std::max(0.0, static_cast<double>(micros) / 1e6);
            }
        }
    }
    bool complete = !health_reports.empty();
    for (const auto& health : health_reports) {
        for (const char* part : {"feedback", "state"}) complete = complete && health.value(part, std::string()) == "available";
    }
    // Newest evidence first; rows without a time sort last, ties keep their first-seen order.
    auto sort_key = [](const json& row) {
        json updated = get_or_null(row, "updated_at");
        return std::make_pair(updated.is_string() ? updated.get<std::string>() : std::string(), row.at("account_id").get<std::string>());
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const json& a, const json& b) { return sort_key(a) > sort_key(b); });
    json out;
    out["generated_at"] = iso_utc_offset(now);
    out["complete"] = complete;
    out["details_available"] = details_available;
    out["sources"] = std::move(health_reports);
    out["items"] = rows;
    return out;
}

}  // namespace ng::order_ui
