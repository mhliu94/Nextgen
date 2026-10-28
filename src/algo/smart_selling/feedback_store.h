// Durable latest-order state and unresolved external command handoffs
// (smart_selling_feedback_store.py). One compact row per broker order and one per
// locally tracked command; never changes the strategy ledger.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/json.h"
#include "core/sqlite.h"
#include "algo/smart_selling/order_status.h"

namespace ng::smart {

inline bool is_feedback_terminal(const std::string& status) {
    return status == "FILLED" || status == "CANCELED" || status == "REJECTED" || status == "EXPIRED";
}

// Upper-cased symbol without US./HK./CN./SH./SZ. prefix and without a '/' suffix.
std::string canonical_order_symbol(const std::string& value);

struct FeedbackRow {
    std::string account_id, order_id, symbol, side, status;
    long long sequence = 0;
    std::string created_at, updated_at;
};

class OrderFeedbackStore {
public:
    explicit OrderFeedbackStore(const std::string& state_path = ":memory:");
    ~OrderFeedbackStore();
    // Records newer evidence; returns the previous compact row if one existed. Throws std::invalid_argument
    // for identity conflicts.
    std::optional<FeedbackRow> record_snapshot(const OrderEvent& event);
    void track_external_order(const json& command);
    bool has_open_orders(const std::string& account_id, const std::string& symbol);
    void close();

private:
    void validate_external_identifiers(const json& payload);
    static void validate_immutable_identity(const FeedbackRow& snapshot, const std::optional<FeedbackRow>& previous);
    static bool is_current_terminal(const FeedbackRow& snapshot, const std::optional<FeedbackRow>& previous);
    void complete_external_commands(const json& payload, const FeedbackRow& snapshot);

    std::recursive_mutex mutex_;
    std::unique_ptr<Sqlite> db_;
};

}  // namespace ng::smart
