#include "algo/session_coordinator.h"

#include <set>
#include <stdexcept>

#include "core/config.h"
#include "core/strings.h"

namespace ng::algo {
namespace {
std::string category(const std::string& mode) {
    if (mode == "E" || mode == "H") return "buying";
    if (mode == "F" || mode == "G") return "selling";
    throw std::invalid_argument("Unknown algorithm mode: " + mode);
}
}

void SessionCoordinator::register_provider(const void* owner, Provider provider) {
    std::lock_guard lock(mutex);
    providers_[owner] = std::move(provider);
}

void SessionCoordinator::unregister_provider(const void* owner) {
    std::lock_guard lock(mutex);
    providers_.erase(owner);
}

void SessionCoordinator::require_available(const std::string& mode, const std::string& symbol,
                                         const std::vector<std::string>& account_ids) {
    std::lock_guard lock(mutex);
    const auto requested_category = category(mode);
    const auto canonical = canonical_symbol(symbol);
    const std::set<std::string> selected(account_ids.begin(), account_ids.end());
    std::vector<std::string> conflicts;
    for (const auto& [owner, provider] : providers_) {
        for (const auto& session : provider()) {
            std::set<std::string> overlap;
            for (const auto& id : session.account_ids) if (selected.count(id)) overlap.insert(id);
            if (overlap.empty()) continue;
            const auto existing_category = category(session.trading_mode);
            const auto existing_symbol = canonical_symbol(session.symbol);
            std::vector<std::string> reasons;
            if (canonical == existing_symbol) reasons.push_back("symbol " + canonical +
                " is already in use; all strategies on an account must use distinct symbols");
            if (requested_category == "buying" && existing_category == "buying")
                reasons.push_back("each account may run at most 1 buying strategy");
            if (!reasons.empty()) conflicts.push_back("Account(s) " +
                join(std::vector<std::string>(overlap.begin(), overlap.end()), ", ") + ": " + join(reasons, "; ") +
                ". Conflicting " + existing_category + " session " + session.session_id +
                " (mode " + session.trading_mode + ", symbol " + existing_symbol + ")");
        }
    }
    if (!conflicts.empty()) throw std::invalid_argument("Cannot start algorithm: " + join(conflicts, ". ") +
        ". Stop the conflicting session and await outstanding order confirmations first.");
}

}  // namespace ng::algo
