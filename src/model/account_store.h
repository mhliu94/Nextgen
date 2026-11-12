// Thread-safe store of the latest account snapshot per account with change notification.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "model/account.h"

namespace ng {

class AccountStore {
public:
    using Listener = std::function<void()>;

    void upsert(AccountSnapshot snapshot);
    void invalidate(const std::string& account_id);
    std::map<std::string, AccountSnapshot> get_all() const;
    std::optional<AccountSnapshot> get(const std::string& account_id) const;
    // Wall-clock time of the last Kafka update, if any.
    std::optional<double> kafka_last_seen() const;
    std::uint64_t version() const;
    // Called (outside the store lock) after every upsert; used by the event hub.
    void add_listener(Listener listener);

private:
    mutable std::shared_mutex mutex_;
    std::map<std::string, AccountSnapshot> accounts_;
    std::optional<double> kafka_last_ts_;
    std::uint64_t version_ = 0;
    std::mutex listeners_mutex_;
    std::vector<Listener> listeners_;
};

}  // namespace ng
