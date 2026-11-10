#include "ops/operations_log.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/logging.h"
#include "core/time_util.h"

namespace ng {

namespace {
constexpr const char* kLog = "trading-ui.operations-log";
}

OperationsLog::OperationsLog(const std::string& log_dir, double server_started_at, SnapshotsProvider provider,
                             std::string source_label)
    : provider_(std::move(provider)), source_label_(std::move(source_label)) {
    started_iso_ = iso_utc(server_started_at);
    std::filesystem::path dir(log_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    path_ = (dir / ("operations_" + filename_timestamp(server_started_at) + ".log")).string();
    ensure_document();
}

void OperationsLog::record_algo_started(const json& command, const std::optional<std::string>& strategy_key,
                                        const std::optional<std::string>& session_id) {
    record_event("algo_trading_started", command, strategy_key, session_id, json::object());
}

void OperationsLog::record_algo_ended(const json& command, const std::optional<std::string>& strategy_key,
                                      const std::optional<std::string>& session_id, const std::optional<std::string>& reason,
                                      std::optional<long long> cycles, std::optional<long long> published_commands) {
    json details = json::object();
    if (reason && !reason->empty()) details["reason"] = *reason;
    if (cycles) details["cycles"] = *cycles;
    if (published_commands) details["published_commands"] = *published_commands;
    record_event("algo_trading_ended", command, strategy_key, session_id, details);
}

void OperationsLog::record_event(const std::string& event_name, const json& command,
                                 const std::optional<std::string>& strategy_key, const std::optional<std::string>& session_id,
                                 const json& details) {
    json event;
    event["event"] = event_name;
    event["recorded_at"] = iso_utc_now();
    event["ui_server_started_at"] = started_iso_;
    event["session"] = session_info(command, strategy_key, session_id);
    event["holdings"] = holdings();
    if (details.is_object() && !details.empty()) event["details"] = details;
    try {
        std::lock_guard lock(mutex_);
        json document = read_document();
        document["events"].push_back(event);
        write_document(document);
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed writing operations log path={} event={}: {}", path_, event_name, e.what());
    }
}

void OperationsLog::ensure_document() {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    if (std::filesystem::exists(path_, ec) && std::filesystem::file_size(path_, ec) > 0) return;
    json document;
    document["ui_server_started_at"] = started_iso_;
    document["events"] = json::array();
    try {
        write_document(document);
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed creating operations log path={}: {}", path_, e.what());
    }
}

json OperationsLog::read_document() const {
    json document;
    try {
        std::ifstream in(path_);
        if (in) {
            std::stringstream buffer;
            buffer << in.rdbuf();
            document = json::parse(buffer.str(), nullptr, false);
        }
    } catch (...) {
        document = json();
    }
    if (!document.is_object()) document = json::object();
    json out;
    const json* started = json_find(document, "ui_server_started_at");
    out["ui_server_started_at"] = (started && started->is_string() && !started->get<std::string>().empty())
                                      ? *started
                                      : json(started_iso_);
    const json* events = json_find(document, "events");
    out["events"] = (events && events->is_array()) ? *events : json::array();
    return out;
}

void OperationsLog::write_document(const json& document) const {
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open " + tmp);
        out << document.dump(2) << "\n";
        if (!out) throw std::runtime_error("cannot write " + tmp);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path_, ec);
    if (ec) throw std::runtime_error("cannot replace " + path_ + ": " + ec.message());
}

json OperationsLog::session_info(const json& command, const std::optional<std::string>& strategy_key,
                                 const std::optional<std::string>& session_id) const {
    json out;
    const json* command_id = json_find(command, "command_id");
    json cid = command_id ? *command_id : json(nullptr);
    if (session_id) out["session_id"] = *session_id;
    else if (!cid.is_null()) out["session_id"] = cid;
    else if (strategy_key) out["session_id"] = *strategy_key;
    else out["session_id"] = nullptr;
    out["command_id"] = cid;
    out["strategy_key"] = strategy_key ? json(*strategy_key) : json(nullptr);
    for (const char* key : {"trading_mode", "symbol", "end_time_et"}) {
        const json* v = json_find(command, key);
        out[key] = v ? *v : json(nullptr);
    }
    if (const json* tm = json_find(command, "fast_trading_test_mode")) out["fast_trading_test_mode"] = json_python_truthy(tm);
    for (const char* key : {"fast_trading_price_limit", "fast_trading_account_ids", "fast_trading_book_levels",
                            "fast_trading_target_percentage", "fast_trading_cycle_seconds",
                            "fast_trading_total_target_position"}) {
        if (const json* v = json_find(command, key)) out[key] = *v;
    }
    return out;
}

json OperationsLog::holdings() const {
    json out;
    if (!provider_) {
        out["source"] = nullptr;
        out["account_count"] = 0;
        out["accounts"] = json::array();
        return out;
    }
    std::map<std::string, AccountSnapshot> accounts;
    try {
        accounts = provider_();
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed loading account snapshots for operations log: {}", e.what());
        out["source"] = nullptr;
        out["error"] = e.what();
        out["account_count"] = 0;
        out["accounts"] = json::array();
        return out;
    }
    std::vector<const AccountSnapshot*> ordered;
    for (auto& [id, snap] : accounts) ordered.push_back(&snap);
    std::sort(ordered.begin(), ordered.end(), [](const AccountSnapshot* a, const AccountSnapshot* b) {
        int na = a->account_num_id.value_or(999999), nb = b->account_num_id.value_or(999999);
        if (na != nb) return na < nb;
        return a->account_id < b->account_id;
    });
    out["source"] = source_label_;
    out["account_count"] = static_cast<long long>(ordered.size());
    out["accounts"] = json::array();
    for (const AccountSnapshot* snap : ordered) {
        json a;
        a["account_id"] = snap->account_id;
        a["account_num_id"] = snap->account_num_id ? json(*snap->account_num_id) : json(nullptr);
        a["cash"] = snap->cash;
        a["cash_by_currency"] = json::object();
        for (auto& [k, v] : snap->cash_by_currency) a["cash_by_currency"][k] = v;
        a["available_cash_by_currency"] = json::object();
        for (auto& [k, v] : snap->available_cash_by_currency) a["available_cash_by_currency"][k] = v;
        std::vector<const Position*> positions;
        for (auto& p : snap->positions) positions.push_back(&p);
        std::sort(positions.begin(), positions.end(),
                  [](const Position* x, const Position* y) { return x->symbol < y->symbol; });
        a["securities"] = json::array();
        for (const Position* p : positions) {
            json s;
            s["symbol"] = p->symbol;
            s["qty"] = p->qty;
            s["available_qty"] = p->available_qty ? json(*p->available_qty) : json(nullptr);
            s["avg_price"] = p->avg_price ? json(*p->avg_price) : json(nullptr);
            a["securities"].push_back(s);
        }
        a["ts"] = snap->ts ? json(*snap->ts) : json(nullptr);
        a["trading_enabled"] = snap->trading_enabled;
        out["accounts"].push_back(a);
    }
    return out;
}

}  // namespace ng
