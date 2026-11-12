#include "model/account_store.h"

#include "core/time_util.h"

namespace ng {

void AccountStore::invalidate(const std::string& account_id) {
    {
        std::unique_lock lock(mutex_);
        auto it=accounts_.find(account_id);
        if (it==accounts_.end()) return;
        it->second.trading_enabled=false;
        it->second.ts=std::nullopt;
        ++version_;
    }
    std::vector<Listener> listeners;
    { std::lock_guard lock(listeners_mutex_); listeners=listeners_; }
    for (auto& listener:listeners) listener();
}

void AccountStore::upsert(AccountSnapshot snapshot) {
    {
        std::unique_lock lock(mutex_);
        accounts_[snapshot.account_id] = std::move(snapshot);
        kafka_last_ts_ = wall_now();
        ++version_;
    }
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(listeners_mutex_);
        listeners = listeners_;
    }
    for (auto& l : listeners) l();
}

std::map<std::string, AccountSnapshot> AccountStore::get_all() const {
    std::shared_lock lock(mutex_);
    return accounts_;
}

std::optional<AccountSnapshot> AccountStore::get(const std::string& account_id) const {
    std::shared_lock lock(mutex_);
    auto it = accounts_.find(account_id);
    if (it == accounts_.end()) return std::nullopt;
    return it->second;
}

std::optional<double> AccountStore::kafka_last_seen() const {
    std::shared_lock lock(mutex_);
    return kafka_last_ts_;
}

std::uint64_t AccountStore::version() const {
    std::shared_lock lock(mutex_);
    return version_;
}

void AccountStore::add_listener(Listener listener) {
    std::lock_guard lock(listeners_mutex_);
    listeners_.push_back(std::move(listener));
}

}  // namespace ng
