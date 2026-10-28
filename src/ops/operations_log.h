// Durable JSON operations log of algorithm start/stop events with account holdings
// (trading_ui/services/operations_log.py). One document per server start.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "core/json.h"
#include "model/account.h"

namespace ng {

class OperationsLog {
public:
    using SnapshotsProvider = std::function<std::map<std::string, AccountSnapshot>()>;

    OperationsLog(const std::string& log_dir, double server_started_at, SnapshotsProvider provider,
                  std::string source_label = "kafka");

    void record_algo_started(const json& command, const std::optional<std::string>& strategy_key,
                             const std::optional<std::string>& session_id);
    void record_algo_ended(const json& command, const std::optional<std::string>& strategy_key,
                           const std::optional<std::string>& session_id, const std::optional<std::string>& reason,
                           std::optional<long long> cycles, std::optional<long long> published_commands);
    // Generic event (used by the web layer for published commands).
    void record_event(const std::string& event_name, const json& command, const std::optional<std::string>& strategy_key,
                      const std::optional<std::string>& session_id, const json& details);

    const std::string& path() const { return path_; }
    const std::string& server_started_at_iso() const { return started_iso_; }
    json read_document() const;

private:
    void ensure_document();
    void write_document(const json& document) const;
    json session_info(const json& command, const std::optional<std::string>& strategy_key,
                      const std::optional<std::string>& session_id) const;
    json holdings() const;

    std::string path_;
    std::string started_iso_;
    SnapshotsProvider provider_;
    std::string source_label_;
    mutable std::mutex mutex_;
};

}  // namespace ng
