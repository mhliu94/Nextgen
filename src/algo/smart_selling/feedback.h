// Map the order-status v1 stream into the Smart selling ledger (smart_selling_feedback.py).
#pragma once

#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "core/json.h"
#include "algo/smart_selling/engine.h"
#include "algo/smart_selling/feedback_store.h"
#include "algo/smart_selling/order_status.h"

namespace ng::smart {

// Validates a raw UTF-8 JSON snapshot, a JSON object or an OrderEvent.
OrderEvent parse_smart_selling_snapshot(const std::string& raw);
OrderEvent parse_smart_selling_snapshot(const json& payload);

class SmartSellingFeedback {
public:
    SmartSellingFeedback(SmartSellingEngine& engine, int max_cached_orders = 10000, const std::string& state_path = ":memory:");

    void track_external_order(const json& command);
    bool has_open_orders(const std::string& account_id, const std::string& symbol);
    void close();
    // Applies matched evidence; returns false for unrelated orders. Throws std::invalid_argument on
    // validation failures (which never apply fills or refunds).
    bool apply_snapshot(const OrderEvent& event);
    bool apply_snapshot(const std::string& raw);
    bool reconcile(const std::string& client_order_id);
    std::optional<json> latest_snapshot(const std::string& client_order_id);

private:
    std::optional<json> find_owned_context(const json& payload);
    static void validate_owned_snapshot(const json& payload, const json& context);
    void hold_identified_conflict(const std::optional<json>& context, const json& payload, const std::string& reason);
    static bool older_report_involves_unknown(const json& payload, const std::optional<FeedbackRow>& previous);
    bool should_ignore_retained_report(const OrderEvent& event, const std::optional<FeedbackRow>& account_previous,
                                       const OrderEvent* cached);
    void apply_cumulative_report(const std::string& client_id, const json& payload);
    void remember_snapshot(const std::string& client_id, const OrderEvent& event);

    SmartSellingEngine& engine_;
    int max_cached_orders_;
    std::recursive_mutex mutex_;
    // LRU of latest events keyed by client order id.
    std::list<std::string> lru_;
    std::map<std::string, std::pair<OrderEvent, std::list<std::string>::iterator>> latest_;
    OrderFeedbackStore store_;
};

}  // namespace ng::smart
