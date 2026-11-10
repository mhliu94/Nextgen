// Algorithm-session reporting hooks shared by fast trading (E/F) and the Smart controllers (G/H).
// The production implementation is reporting::ActivityReporting; tests inject recorders.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace ng::reporting {

class SessionReporter {
public:
    virtual ~SessionReporter() = default;
    // Records a new session once (later calls with the same id are ignored). `started_at` is an
    // ISO-8601 timestamp with an offset; the current time is used when absent.
    virtual void start_session(const std::string& session_id, const json& parameters,
                               const std::vector<std::string>& account_ids,
                               const std::optional<std::string>& started_at = std::nullopt) = 0;
    // Refreshes the liveness timestamp (and status) of a session that has not ended.
    virtual void heartbeat_session(const std::string& session_id, const std::string& status = "running") = 0;
    virtual void end_session(const std::string& session_id, const std::string& status = "stopped",
                             const std::optional<std::string>& reason = std::nullopt) = 0;
    // Associates an order command (command_id / client_order_id) of one account with a session, so
    // confirmed order snapshots can be attributed to it. Throws std::invalid_argument when the
    // command already belongs to another session.
    virtual void register_command(const std::string& session_id, const json& command) = 0;
};

}  // namespace ng::reporting
