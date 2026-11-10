// The order-status.v1 contract lives in orders/order_status.h; these aliases keep the Smart
// controllers' code reading naturally.
#pragma once

#include "orders/order_status.h"

namespace ng::smart {

using orders::kMaxOrderMessageBytes;
using orders::OrderEvent;
using orders::parse_order_message;
using orders::parse_order_payload;

}  // namespace ng::smart
