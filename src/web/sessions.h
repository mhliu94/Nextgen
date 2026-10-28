// In-memory login sessions (token -> user) with constant-time password verification.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "core/config.h"

namespace ng::web {

class SessionStore {
public:
    explicit SessionStore(double ttl_seconds = 12 * 3600);
    bool verify_credentials(const std::map<std::string, AuthUser>& users, const std::string& username,
                            const std::string& password) const;
    std::string create(const std::string& user);
    std::optional<std::string> user_for(const std::string& token);
    void revoke(const std::string& token);
    void clear();
    std::size_t size();
    // Tests only.
    void insert_for_test(const std::string& token, const std::string& user);

private:
    struct Entry {
        std::string user;
        double expires_at;
    };
    void purge_expired_locked(double now);

    double ttl_;
    std::mutex mutex_;
    std::map<std::string, Entry> sessions_;
};

// Constant-time comparison (hmac.compare_digest).
bool compare_digest(const std::string& a, const std::string& b);

}  // namespace ng::web
