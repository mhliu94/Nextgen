#include "ops/reconcile.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>

#include "algo/smart_buying/engine.h"
#include "algo/smart_selling/engine.h"
#include "algo/smart_selling/feedback.h"
#include "algo/smart_selling/feedback_store.h"
#include "core/sqlite.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "core/time_zone.h"

namespace ng::ops {
namespace {

namespace fs = std::filesystem;

// Path.resolve(): absolute with symlinks resolved where the path exists.
fs::path resolve(const std::string& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    fs::path canonical = fs::weakly_canonical(absolute, ec);
    return ec ? absolute.lexically_normal() : canonical;
}

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// datetime.fromisoformat(value.replace("Z", "+00:00")) with a mandatory offset.
double timestamp(const std::string& value) {
    auto parsed = parse_iso8601(value);
    if (!parsed) throw std::invalid_argument("Invalid isoformat string: " + value);
    if (!parsed->has_offset) throw std::invalid_argument("Timestamp must include a timezone");
    return parsed->epoch_seconds;
}

// The UTC time embedded in a generated command id ("limit_2026-10-26T15:32:47.123Z_1"), if any.
std::optional<std::string> command_time(const std::string& command_id) {
    static const std::regex pattern(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z)");
    std::smatch match;
    if (!std::regex_search(command_id, match, pattern)) return std::nullopt;
    return match.str(0);
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

struct Connections {
    std::vector<std::unique_ptr<Sqlite>> items;
    ~Connections() {
        // Release in reverse order; uncommitted work is rolled back.
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            try {
                if ((*it)->in_transaction()) (*it)->rollback();
            } catch (const std::exception&) {
            }
            it->reset();
        }
    }
};

}  // namespace

json reconcile_prior_day(const std::vector<ReconcileSource>& sources, const std::string& timezone_name, const std::string& reason,
                         const std::optional<std::string>& backup_dir, bool apply, std::optional<double> now_in) {
    if (python_strip(reason).empty()) throw std::invalid_argument("An operator reason is required");
    double now = now_in ? *now_in : wall_now();
    auto zone = TimeZone::find(timezone_name);
    if (!zone) throw std::invalid_argument("Unknown time zone: " + timezone_name);
    std::string trading_day = zone->date(now);

    std::vector<fs::path> paths, all_paths;
    for (const auto& source : sources) paths.push_back(resolve(source.state_db));
    all_paths = paths;
    for (const auto& path : paths) all_paths.push_back(fs::path(path.string() + ".feedback.sqlite3"));
    bool valid = !paths.empty() && std::set<fs::path>(all_paths.begin(), all_paths.end()).size() == all_paths.size();
    for (const auto& path : all_paths) valid = valid && is_file(path);
    if (!valid) throw std::invalid_argument("Existing, separate state and feedback databases are required");
    std::optional<fs::path> backup;
    if (backup_dir && !backup_dir->empty()) backup = resolve(*backup_dir);
    if (apply && !backup) throw std::invalid_argument("A backup directory is required when applying reconciliation");
    if (apply) {
        std::error_code ec;
        if (fs::exists(*backup, ec)) throw std::invalid_argument("Backup directory already exists: " + backup->string());
        fs::create_directories(*backup, ec);
        if (ec) throw std::runtime_error("cannot create backup directory " + backup->string() + ": " + ec.message());
        for (std::size_t index = 0; index < all_paths.size(); ++index) {
            Sqlite source(all_paths[index].string(), Sqlite::Mode::ReadOnly);
            Sqlite target((*backup / (std::to_string(index) + "-" + all_paths[index].filename().string())).string());
            source.backup_to(target);
        }
    }

    Connections connections;
    // Consistent lock order with the stopped-child repair utility: state stores first.
    for (const auto& path : all_paths) {
        auto db = std::make_unique<Sqlite>(path.string(), apply ? Sqlite::Mode::ReadWrite : Sqlite::Mode::ReadOnly, 10000);
        if (apply) db->begin_immediate();
        else db->begin();
        connections.items.push_back(std::move(db));
    }
    std::vector<json> active_children;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        Statement rows(*connections.items[i], "SELECT data FROM smart_parents WHERE active=1");
        while (rows.step()) {
            json parent = json::parse(rows.column_text(0));
            const json* child = json_find(parent, "order");
            if (child && child->is_object() && child->value("state", json(nullptr)) != "TERMINAL") active_children.push_back(*child);
        }
    }

    json plans = json::array();
    for (std::size_t i = 0; i < sources.size(); ++i) {
        Sqlite& db = *connections.items[paths.size() + i];
        json plan = {{"mode", sources[i].mode}, {"orders", json::array()}, {"commands", json::array()},
                     {"undated_commands", json::array()}};
        {
            Statement rows(db, "SELECT * FROM feedback_orders WHERE status NOT IN ('FILLED','CANCELED','REJECTED','EXPIRED')");
            while (rows.step()) {
                json row = rows.row_json();
                if (zone->date(timestamp(row.at("created_at").get<std::string>())) >= trading_day) continue;
                for (const json& child : active_children) {
                    if (child.value("account_id", json(nullptr)) == row.at("account_id") &&
                        child.value("broker_order_id", json(nullptr)) == row.at("order_id")) {
                        throw std::invalid_argument(
                            "A prior-day broker order belongs to an active algorithm child; reconcile that child first");
                    }
                }
                plan["orders"].push_back(std::move(row));
            }
        }
        {
            Statement rows(db, "SELECT * FROM feedback_pending_commands WHERE pending=1");
            while (rows.step()) {
                json row = rows.row_json();
                std::string command_id = row.at("command_id").get<std::string>();
                auto dated = command_time(command_id);
                if (!dated) {
                    plan["undated_commands"].push_back(std::move(row));
                    continue;
                }
                if (zone->date(timestamp(*dated)) >= trading_day) continue;
                for (const json& child : active_children) {
                    if (child.value("client_order_id", json(nullptr)) == command_id) {
                        throw std::invalid_argument(
                            "A prior-day command belongs to an active algorithm child; reconcile that child first");
                    }
                }
                plan["commands"].push_back(std::move(row));
            }
        }
        // Never silently leave an old dispatched child outside the cleanup.
        for (const json& child : active_children) {
            const json* sent = json_find(child, "send_wall");
            if (sent && sent->is_number() && zone->date(sent->get<double>()) < trading_day) {
                throw std::invalid_argument("A prior-day active algorithm child requires separate reconciliation");
            }
        }
        plans.push_back(std::move(plan));
    }

    std::string as_of = iso_utc_offset(now);
    json result = {{"applied", apply}, {"as_of", as_of}, {"trading_day", trading_day}, {"timezone", timezone_name},
                   {"reason", reason}, {"stores", plans}};
    if (apply) {
        json preview = result;
        preview["applied"] = false;
        write_text(*backup / "preview.json", preview.dump(2) + "\n");
        for (std::size_t i = 0; i < sources.size(); ++i) {
            Sqlite& db = *connections.items[paths.size() + i];
            const json& plan = plans[i];
            // Schema, watermark, deletion and audit stay in one transaction per feedback file.
            db.exec(smart::kReconciliationSchema);
            for (const json& row : plan.at("orders")) {
                Statement insert(db, "INSERT OR REPLACE INTO feedback_reconciled_orders VALUES (?,?,?,?,?,?,?,?,?)");
                insert.bind(1, row.at("account_id").get<std::string>()).bind(2, row.at("order_id").get<std::string>());
                insert.bind(3, row.at("sequence").get<long long>()).bind(4, row.at("created_at").get<std::string>());
                insert.bind(5, row.at("symbol").get<std::string>()).bind(6, row.at("side").get<std::string>());
                insert.bind(7, as_of).bind(8, reason).bind(9, row.dump());
                insert.exec();
                Statement remove(db, "DELETE FROM feedback_orders WHERE account_id=? AND order_id=?");
                remove.bind(1, row.at("account_id").get<std::string>()).bind(2, row.at("order_id").get<std::string>());
                remove.exec();
            }
            for (const json& row : plan.at("commands")) {
                Statement update(db, "UPDATE feedback_pending_commands SET pending=0 WHERE command_id=?");
                update.bind(1, row.at("command_id").get<std::string>());
                update.exec();
            }
            if (!plan.at("orders").empty() || !plan.at("commands").empty()) {
                Statement audit(db, "INSERT INTO feedback_reconciliation_audit (reconciled_at,trading_day,timezone,reason,data) "
                                    "VALUES (?,?,?,?,?)");
                audit.bind(1, as_of).bind(2, trading_day).bind(3, timezone_name).bind(4, reason).bind(5, plan.dump());
                audit.exec();
            }
        }
        // Each feedback file commits atomically. The preview and backups support recovery if a host
        // failure interrupts between files.
        for (std::size_t i = paths.size(); i < connections.items.size(); ++i) connections.items[i]->commit();
        write_text(*backup / "result.json", result.dump(2) + "\n");
    }
    return result;
}

namespace {

// An in-memory engine that receives one parent and its outstanding child, runs the ordinary terminal
// transition and exposes the rows it changed. Opening a second live engine on the journal would run
// restart recovery and disturb unrelated sessions and pending outbox entries.
template <typename Engine>
class RepairEngine : public Engine {
public:
    RepairEngine() : Engine(":memory:") {}

    void import_rows(const std::string& parent_id, const std::string& account_id, const std::string& symbol, long long active,
                     const std::string& parent_data, const std::string& client_order_id, const std::string& order_data) {
        std::lock_guard lock(this->mutex_);
        Statement parent(*this->db_, "INSERT INTO smart_parents VALUES (?,?,?,?,?)");
        parent.bind(1, parent_id).bind(2, account_id).bind(3, symbol).bind(4, active).bind(5, parent_data);
        parent.exec();
        Statement order(*this->db_, "INSERT INTO smart_orders VALUES (?,?,?)");
        order.bind(1, client_order_id).bind(2, parent_id).bind(3, order_data);
        order.exec();
    }
    void audit(const std::string& parent_id, const json& detail) {
        std::lock_guard lock(this->mutex_);
        this->db_->begin_immediate();
        try {
            this->event(this->load(parent_id), "manual_terminal_reconciliation", detail);
            this->db_->commit();
        } catch (...) {
            this->db_->rollback();
            throw;
        }
    }
    // (active, data) of the parent, the child's data, and every journal event of the transition.
    std::pair<long long, std::string> parent_row(const std::string& parent_id) {
        std::lock_guard lock(this->mutex_);
        Statement row(*this->db_, "SELECT active,data FROM smart_parents WHERE id=?");
        row.bind(1, parent_id);
        if (!row.step()) throw std::runtime_error("repair engine lost the parent");
        return {row.column_int(0), row.column_text(1)};
    }
    std::string order_row(const std::string& client_order_id) {
        std::lock_guard lock(this->mutex_);
        Statement row(*this->db_, "SELECT data FROM smart_orders WHERE id=?");
        row.bind(1, client_order_id);
        if (!row.step()) throw std::runtime_error("repair engine lost the child");
        return row.column_text(0);
    }
    struct Event {
        std::string parent_id, kind, data;
        double ts;
    };
    std::vector<Event> events_rows() {
        std::lock_guard lock(this->mutex_);
        std::vector<Event> out;
        Statement rows(*this->db_, "SELECT parent_id,ts,kind,data FROM smart_events ORDER BY id");
        while (rows.step()) out.push_back({rows.column_text(0), rows.column_text(2), rows.column_text(3), rows.column_double(1)});
        return out;
    }
};

struct RepairOutcome {
    json result;
    std::pair<long long, std::string> parent_row;
    std::string order_row;
    std::vector<std::tuple<std::string, double, std::string, std::string>> events;
};

template <typename Engine>
RepairOutcome run_repair(const std::string& parent_id, const std::string& account_id, const std::string& symbol, long long active,
                         const std::string& parent_data, const std::string& client_order_id, const std::string& order_data,
                         const json& payload, const json& audit) {
    RepairEngine<Engine> engine;
    engine.import_rows(parent_id, account_id, symbol, active, parent_data, client_order_id, order_data);
    auto context = engine.get_order_context(client_order_id);
    if (!context) throw std::invalid_argument("Unknown Smart selling child");
    smart::SmartSellingFeedback::validate_owned_snapshot(payload, *context);
    smart::OrderReport report;
    report.status = payload.at("status").get<std::string>();
    report.cumulative_filled = payload.at("filled_quantity");
    report.broker_order_id = payload.at("order_id").get<std::string>();
    report.sequence = payload.at("sequence").get<long long>();
    report.authoritative = true;
    report.final = true;
    engine.on_order_report(client_order_id, report);
    RepairOutcome outcome;
    outcome.result = engine.get_parent(parent_id);
    if (outcome.result.at("active").get<bool>() || !outcome.result.at("active_order").is_null() ||
        outcome.result.at("reserved") != "0") {
        throw std::invalid_argument("Terminal evidence did not resolve the stopped session");
    }
    engine.audit(parent_id, audit);
    outcome.parent_row = engine.parent_row(parent_id);
    outcome.order_row = engine.order_row(client_order_id);
    for (auto& event : engine.events_rows()) outcome.events.emplace_back(event.parent_id, event.ts, event.kind, event.data);
    engine.close();
    return outcome;
}

}  // namespace

json reconcile_stopped_child(const std::string& state_db, const std::string& feedback_db, const std::string& client_order_id,
                             const json& snapshot, const std::string& source, bool apply) {
    if (python_strip(source).empty()) throw std::invalid_argument("A source reference for the reviewed evidence is required");
    smart::OrderEvent event = smart::parse_smart_selling_snapshot(snapshot);
    const json& payload = event.payload;
    std::string status = payload.at("status").get<std::string>();
    if (!smart::is_feedback_terminal(status)) throw std::invalid_argument("Authoritative terminal broker status is required");
    for (const char* field : {"command_id", "client_order_id"}) {
        const json* value = json_find(payload, field);
        if (json_python_truthy(value) && *value != client_order_id) {
            throw std::invalid_argument("Snapshot identifies a different client order");
        }
    }
    fs::path state_path = resolve(state_db), feedback_path = resolve(feedback_db);
    if (!is_file(state_path) || !is_file(feedback_path) || state_path == feedback_path) {
        throw std::invalid_argument("Existing, separate state and feedback databases are required");
    }

    Sqlite db(state_path.string(), Sqlite::Mode::ReadWrite, 10000);
    {
        Statement attach(db, "ATTACH DATABASE ? AS feedback");
        attach.bind(1, feedback_path.string());
        attach.exec();
    }
    db.begin_immediate();
    try {
        std::string parent_id, order_data;
        {
            Statement row(db, "SELECT parent_id,data FROM smart_orders WHERE id=?");
            row.bind(1, client_order_id);
            if (!row.step()) throw std::invalid_argument("Unknown Smart selling child");
            parent_id = row.column_text(0);
            order_data = row.column_text(1);
        }
        std::string account_id, symbol, parent_data;
        long long active = 0;
        {
            Statement row(db, "SELECT id,account_id,symbol,active,data FROM smart_parents WHERE id=?");
            row.bind(1, parent_id);
            if (!row.step()) throw std::invalid_argument("Unknown Smart selling child");
            account_id = row.column_text(1);
            symbol = row.column_text(2);
            active = row.column_int(3);
            parent_data = row.column_text(4);
        }
        json parent = json::parse(parent_data);
        const json* child = json_find(parent, "order");
        bool outstanding = json_python_truthy(json_find(parent, "active")) && json_python_truthy(json_find(parent, "stop_requested")) &&
                           child && child->is_object() && child->value("client_order_id", json(nullptr)) == client_order_id;
        if (!outstanding) throw std::invalid_argument("Only an outstanding child of a stopped session can be reconciled");
        if (json_python_truthy(json_find(parent, "hold")) || !json_python_truthy(json_find(*child, "dispatch_prepared"))) {
            throw std::invalid_argument("Conflicting or undispatched children require separate investigation");
        }
        const json& bound = child->at("broker_order_id");
        if (!bound.is_null() && bound != payload.at("order_id")) {
            throw std::invalid_argument("Child is already bound to a different broker order");
        }
        if (event.created_at < child->at("send_wall").get<double>()) {
            throw std::invalid_argument("Broker order predates the child submission");
        }
        {
            Statement other(db, "SELECT child.id FROM smart_orders child "
                                "JOIN smart_parents parent ON parent.id=child.parent_id "
                                "WHERE COALESCE(json_extract(child.data,'$.account_id'),parent.account_id)=? "
                                "AND json_extract(child.data,'$.broker_order_id')=? AND child.id<>?");
            other.bind(1, payload.at("account_id").get<std::string>()).bind(2, payload.at("order_id").get<std::string>());
            other.bind(3, client_order_id);
            if (other.step()) throw std::invalid_argument("Broker order is already bound to another child");
        }
        {
            Statement recorded(db, "SELECT symbol,side,status,sequence,created_at,updated_at FROM feedback.feedback_orders "
                                   "WHERE account_id=? AND order_id=?");
            recorded.bind(1, payload.at("account_id").get<std::string>()).bind(2, payload.at("order_id").get<std::string>());
            bool matches = recorded.step() &&
                           recorded.column_text(0) == smart::canonical_order_symbol(payload.at("symbol").get<std::string>()) &&
                           recorded.column_text(1) == payload.at("side").get<std::string>() && recorded.column_text(2) == status &&
                           recorded.column_int(3) == payload.at("sequence").get<long long>() &&
                           recorded.column_text(4) == iso_utc_offset(event.created_at) &&
                           recorded.column_text(5) == iso_utc_offset(event.updated_at);
            if (!matches) throw std::invalid_argument("Snapshot must match the latest durable broker feedback");
        }

        json previous_ledger = json::object();
        for (const char* key : {"E", "C", "R", "F", "D"}) previous_ledger[key] = parent.at(key);
        json audit = {{"source", source}, {"client_order_id", client_order_id}, {"broker_order_id", payload.at("order_id")},
                      {"snapshot", payload}, {"previous_order", *child}, {"previous_ledger", previous_ledger}};
        // Smart buying parents carry a price ceiling and cash budgets; their fills also consume cash.
        bool buying = parent.contains("maximum_price");
        RepairOutcome outcome =
            buying ? run_repair<smart::SmartBuyingEngine>(parent_id, account_id, symbol, active, parent_data, client_order_id,
                                                          order_data, payload, audit)
                   : run_repair<smart::SmartSellingEngine>(parent_id, account_id, symbol, active, parent_data, client_order_id,
                                                           order_data, payload, audit);
        if (apply) {
            Statement update_parent(db, "UPDATE smart_parents SET active=?,data=? WHERE id=?");
            update_parent.bind(1, outcome.parent_row.first).bind(2, outcome.parent_row.second).bind(3, parent_id);
            update_parent.exec();
            Statement update_order(db, "UPDATE smart_orders SET data=? WHERE id=?");
            update_order.bind(1, outcome.order_row).bind(2, client_order_id);
            update_order.exec();
            for (const auto& [event_parent, ts, kind, data] : outcome.events) {
                Statement insert(db, "INSERT INTO smart_events(parent_id,ts,kind,data) VALUES (?,?,?,?)");
                insert.bind(1, event_parent).bind(2, ts).bind(3, kind).bind(4, data);
                insert.exec();
            }
            db.commit();
        } else {
            db.rollback();
        }
        return json{{"applied", apply}, {"parent", outcome.result}, {"source", source}};
    } catch (...) {
        if (db.in_transaction()) db.rollback();
        throw;
    }
}

}  // namespace ng::ops
