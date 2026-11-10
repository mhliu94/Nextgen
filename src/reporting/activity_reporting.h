// Durable order, tag and algorithm reports shared by the control server and the order UI
// (trading_ui/services/activity_reporting.py). The two processes use the same SQLite file
// (WAL); the schema and row formats are identical to the Python implementation.
//
// Executions come exclusively from cumulative broker order snapshots. Commands only establish
// session ownership; their requested quantities are never fills.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.h"
#include "core/sqlite.h"
#include "model/account.h"
#include "orders/order_status.h"
#include "reporting/session_reporter.h"

namespace ng::reporting {

// Python KeyError equivalent (unknown tag id).
class KeyError : public std::out_of_range {
public:
    using std::out_of_range::out_of_range;
};

class ActivityReporting : public SessionReporter {
public:
    using Clock = std::function<double()>;  // wall-clock epoch seconds

    explicit ActivityReporting(const std::string& db_path, Clock now = nullptr);
    ~ActivityReporting() override;
    void close();

    void start_session(const std::string& session_id, const json& parameters, const std::vector<std::string>& account_ids,
                       const std::optional<std::string>& started_at = std::nullopt) override;
    void heartbeat_session(const std::string& session_id, const std::string& status = "running") override;
    void end_session(const std::string& session_id, const std::string& status = "stopped",
                     const std::optional<std::string>& reason = std::nullopt) override;
    void register_command(const std::string& session_id, const json& command) override;

    // Accepts a validated order event, independently of the recent-orders date. Returns false for
    // an equal or older sequence. Throws std::invalid_argument when immutable fields changed.
    bool record_order(const orders::OrderEvent& event);

    // Starts a symbol tag and returns its id. Throws std::invalid_argument for invalid input or when
    // another tag is active (the message then contains "active tag").
    std::string start_tag(const std::string& name, const std::string& symbol);
    // Closes the tag's period (idempotent). Throws KeyError for an unknown id.
    void stop_tag(const std::string& tag_id);
    // Tags attached to each order: keyed by (account_id, order_id), values are [{id, name}] arrays.
    std::map<std::pair<std::string, std::string>, json> order_tags(const std::vector<json>& orders);
    // Session and tag reports restricted to the given (visible) accounts.
    json snapshot(const AccountDirectory& accounts);

private:
    template <typename Fn>
    auto transaction(bool durable, Fn&& fn) -> decltype(fn());

    Clock now_;
    std::recursive_mutex mutex_;
    std::unique_ptr<Sqlite> db_;
};

// Aggregates confirmed executions per account and overall (activity_reporting._summarize).
// `include_accounts` lists accounts that appear even without executions.
json summarize_executions(const std::vector<json>& orders, const AccountDirectory& metas,
                          const std::vector<std::string>& include_accounts = {});

}  // namespace ng::reporting
