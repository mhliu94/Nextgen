// Test doubles for the algorithm-session reporting hooks.
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "reporting/session_reporter.h"

namespace ngtest {

// Records every reporting call as {method, session_id, ...} JSON objects, in call order.
class RecordingReporter : public ng::reporting::SessionReporter {
public:
    void start_session(const std::string& session_id, const ng::json& parameters, const std::vector<std::string>& account_ids,
                       const std::optional<std::string>& started_at = std::nullopt) override {
        std::lock_guard lock(mutex_);
        if (fail_method_ == "start_session") throw std::runtime_error("reporting unavailable");
        calls_.push_back(ng::json{{"method", "start_session"}, {"session_id", session_id}, {"parameters", parameters},
                                  {"account_ids", account_ids}, {"started_at", started_at ? ng::json(*started_at) : ng::json(nullptr)}});
    }
    void heartbeat_session(const std::string& session_id, const std::string& status = "running") override {
        std::lock_guard lock(mutex_);
        if (fail_method_ == "heartbeat_session") throw std::runtime_error("reporting unavailable");
        calls_.push_back(ng::json{{"method", "heartbeat_session"}, {"session_id", session_id}, {"status", status}});
    }
    void end_session(const std::string& session_id, const std::string& status = "stopped",
                     const std::optional<std::string>& reason = std::nullopt) override {
        std::lock_guard lock(mutex_);
        if (fail_method_ == "end_session") throw std::runtime_error("reporting unavailable");
        calls_.push_back(ng::json{{"method", "end_session"}, {"session_id", session_id}, {"status", status},
                                  {"reason", reason ? ng::json(*reason) : ng::json(nullptr)}});
    }
    void register_command(const std::string& session_id, const ng::json& command) override {
        std::lock_guard lock(mutex_);
        if (fail_method_ == "register_command") throw std::runtime_error("reporting unavailable");
        calls_.push_back(ng::json{{"method", "register_command"}, {"session_id", session_id}, {"command", command}});
    }

    std::vector<ng::json> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    std::vector<ng::json> calls(const std::string& method) const {
        std::lock_guard lock(mutex_);
        std::vector<ng::json> out;
        for (const auto& call : calls_) {
            if (call.at("method") == method) out.push_back(call);
        }
        return out;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        calls_.clear();
    }
    // Makes the named method throw (reporting failures must never interrupt trading).
    void fail(const std::string& method) {
        std::lock_guard lock(mutex_);
        fail_method_ = method;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ng::json> calls_;
    std::string fail_method_;
};

}  // namespace ngtest
