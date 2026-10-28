// Translate controller intents into the existing trading-command vocabulary (smart_selling_commands.py).
#pragma once

#include "core/json.h"
#include "model/account.h"

namespace ng::smart {

// Throws std::invalid_argument when a quantity/price cannot be represented exactly on the wire.
json build_smart_selling_command(const json& action, const AccountDirectory& accounts);

}  // namespace ng::smart
