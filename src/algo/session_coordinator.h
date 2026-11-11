// Shared admission rules for fast and Smart sessions (KTrader algo_sessions.py).
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ng::algo {

struct Session {
    std::string session_id, trading_mode, symbol;
    std::vector<std::string> account_ids;
};

class SessionCoordinator {
public:
    using Provider = std::function<std::vector<Session>()>;
    void register_provider(const void* owner, Provider provider);
    void unregister_provider(const void* owner);
    // Hold mutex through this check AND registration in the runtime. Providers include
    // scheduled, paused, stopping and reconciling sessions, including durable recovery.
    void require_available(const std::string& mode, const std::string& symbol,
                           const std::vector<std::string>& account_ids);
    std::recursive_mutex mutex;

private:
    std::map<const void*, Provider> providers_;
};

}  // namespace ng::algo
