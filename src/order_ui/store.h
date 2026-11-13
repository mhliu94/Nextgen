// Thread-safe projection of today's latest order snapshots (order_ui/store.py).
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "core/json.h"
#include "core/time_zone.h"
#include "model/account.h"
#include "orders/order_status.h"

namespace ng::order_ui {

class OrderStore {
public:
    using Clock = std::function<double()>;  // wall-clock epoch seconds
    // Receives every accepted event created today or earlier (reports span dates; late fills on a
    // tagged order remain reportable). May throw std::invalid_argument to reject the event.
    // Runs in ingestion order, without holding the snapshot/feed mutex.
    using Reporting = std::function<void(const orders::OrderEvent& event)>;

    // Throws std::invalid_argument for an unsupported time zone.
    OrderStore(AccountDirectory accounts, const std::string& timezone_name, Clock now = nullptr, Reporting reporting = nullptr);

    void update_feed(const std::optional<std::string>& state, const std::optional<std::string>& detail);
    // Marks the arrival of a Kafka message.
    void received();
    json feed_status();
    void reject_message();
    // Applies one validated snapshot. Returns true when today's projection changed; throws
    // std::invalid_argument when immutable order fields changed.
    bool upsert(const orders::OrderEvent& event);
    // {date, timezone, generated_at, orders (newest update first), feed}.
    json snapshot();

    const AccountDirectory& accounts() const { return accounts_; }
    const std::string& timezone() const { return zone_.name(); }

private:
    std::string rollover();

    AccountDirectory accounts_;
    TimeZone zone_;
    Clock now_;
    Reporting reporting_;
    std::recursive_mutex upsert_mutex_;
    std::recursive_mutex mutex_;
    std::map<std::pair<std::string, std::string>, orders::OrderEvent> orders_;
    std::string day_;
    std::string feed_state_ = "starting";
    std::string feed_detail_ = "Connecting to Kafka";
    std::optional<std::string> last_message_at_;
    long long invalid_messages_ = 0;
    long long ignored_messages_ = 0;
};

}  // namespace ng::order_ui
