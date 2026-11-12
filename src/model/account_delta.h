#pragma once
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include "core/json.h"

namespace ng {
// Version 1 account stream: changed values replace fields; they are not increments.
class AccountReplica {
public:
    std::optional<json> apply(const json& event);
    bool needs_snapshot() const { return missing_; }
private:
    std::optional<json> state_;
    std::string stream_;
    std::int64_t sequence_ = 0;
    std::set<std::string> retired_;
    bool missing_ = false;
};
}
