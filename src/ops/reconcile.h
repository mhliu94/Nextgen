// Operator reconciliation utilities (trading_ui/reconcile_prior_day.py and
// trading_ui/reconcile_smart_selling.py). Both preview by default; neither submits, cancels or
// publishes an order, invents a broker status or restarts a service.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace ng::ops {

// One Smart controller journal: "G" (Smart selling) or "H" (Smart buying). Its feedback store is
// "<state_db>.feedback.sqlite3".
struct ReconcileSource {
    std::string mode;
    std::string state_db;
};

// Marks external orders and command handoffs dated before the current trading day as reconciled,
// preserving current/undated records and history. With `apply` the feedback stores are backed up
// into `backup_dir` (which must not exist yet) and updated atomically per file; without it nothing
// is changed. Active algorithm children are never touched: they refuse the cleanup.
// Throws std::invalid_argument with the Python tool's messages.
json reconcile_prior_day(const std::vector<ReconcileSource>& sources, const std::string& timezone_name, const std::string& reason,
                         const std::optional<std::string>& backup_dir = std::nullopt, bool apply = false,
                         std::optional<double> now = std::nullopt);

// Resolves the outstanding child of a *stopped* session against reviewed terminal broker evidence
// that is already the latest durable feedback for that broker order. Requires an explicit client to
// broker order mapping; ownership is never guessed from symbol, price, quantity or time. The normal
// engine transition runs on an in-memory copy, so unrelated sessions and the outbox are untouched.
json reconcile_stopped_child(const std::string& state_db, const std::string& feedback_db, const std::string& client_order_id,
                             const json& snapshot, const std::string& source, bool apply = false);

}  // namespace ng::ops
