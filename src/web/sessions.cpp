#include "web/sessions.h"

#include <openssl/crypto.h>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng::web {

bool compare_digest(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        // Still perform a comparison to keep timing roughly constant.
        volatile int sink = CRYPTO_memcmp(a.data(), a.data(), a.size());
        (void)sink;
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

SessionStore::SessionStore(double ttl_seconds) : ttl_(ttl_seconds) {}

bool SessionStore::verify_credentials(const std::map<std::string, AuthUser>& users, const std::string& username,
                                      const std::string& password) const {
    auto it = users.find(username);
    if (it == users.end()) return false;
    return compare_digest(it->second.password, password);
}

std::string SessionStore::create(const std::string& user) {
    std::string token = random_token(32);
    std::lock_guard lock(mutex_);
    double now = wall_now();
    purge_expired_locked(now);
    sessions_[token] = Entry{user, now + ttl_};
    return token;
}

std::optional<std::string> SessionStore::user_for(const std::string& token) {
    if (token.empty()) return std::nullopt;
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return std::nullopt;
    if (it->second.expires_at < wall_now()) {
        sessions_.erase(it);
        return std::nullopt;
    }
    return it->second.user;
}

void SessionStore::revoke(const std::string& token) {
    std::lock_guard lock(mutex_);
    sessions_.erase(token);
}

void SessionStore::clear() {
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

std::size_t SessionStore::size() {
    std::lock_guard lock(mutex_);
    purge_expired_locked(wall_now());
    return sessions_.size();
}

void SessionStore::insert_for_test(const std::string& token, const std::string& user) {
    std::lock_guard lock(mutex_);
    sessions_[token] = Entry{user, wall_now() + ttl_};
}

void SessionStore::purge_expired_locked(double now) {
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.expires_at < now) it = sessions_.erase(it);
        else ++it;
    }
}

}  // namespace ng::web
