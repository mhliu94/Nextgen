// Validation of order-status.v1 snapshots (order_ui/schema/order-status.v1.schema.json plus the
// cross-field rules in order_ui/events.py). This is the consumer-side runtime contract.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "core/json.h"

namespace ng::smart {

struct OrderEvent {
    json payload;
    double created_at = 0.0;  // epoch seconds
    double updated_at = 0.0;
    std::string created_at_iso;  // UTC ISO 8601 of created_at
    std::string updated_at_iso;
};

// Throws std::invalid_argument with a short, payload-free message when the snapshot is invalid.
OrderEvent parse_order_message(std::string_view raw);
OrderEvent parse_order_payload(const json& payload);

inline constexpr std::size_t kMaxOrderMessageBytes = 65536;

}  // namespace ng::smart
