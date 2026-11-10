// Read-only view of the Smart controllers' unresolved records (order_ui/reconciliation.py): open
// broker orders, unconfirmed command handoffs and outstanding algorithm children, across all dates.
// Nothing here changes a ledger, cancels an order or clears an account lock.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/json.h"
#include "model/account.h"
#include "order_ui/config.h"

namespace ng::order_ui {

class ReconciliationView {
public:
    using Clock = std::function<double()>;  // wall-clock epoch seconds

    ReconciliationView(std::vector<ReconciliationSource> sources, std::string reporting_db_path, Clock now = nullptr);

    // {generated_at, complete, details_available, sources: [{mode, feedback, state}], items: [...]},
    // newest evidence first, restricted to the given (visible) accounts. Databases are opened
    // read-only; a missing or unreadable store is reported, never created.
    json snapshot(const AccountDirectory& accounts) const;

private:
    std::vector<ReconciliationSource> sources_;
    std::string reporting_db_path_;
    Clock now_;
};

}  // namespace ng::order_ui
