#include "order_ui/store.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

#include "core/time_util.h"

namespace ng::order_ui {
namespace {

TimeZone zone_or_throw(const std::string& name) {
    auto zone = TimeZone::find(name);
    if (!zone) throw std::invalid_argument("Unknown order_ui.timezone: " + name);
    return *zone;
}

long long sequence_of(const json& payload) {
    const json& value = payload.at("sequence");
    return value.is_number_float() ? static_cast<long long>(value.get<double>()) : value.get<long long>();
}

}  // namespace

OrderStore::OrderStore(AccountDirectory accounts, const std::string& timezone_name, Clock now, Reporting reporting)
    : accounts_(std::move(accounts)),
      zone_(zone_or_throw(timezone_name)),
      now_(now ? std::move(now) : Clock(&wall_now)),
      reporting_(std::move(reporting)) {
    day_ = zone_.date(now_());
}

std::string OrderStore::rollover() {
    std::string today = zone_.date(now_());
    if (today != day_) {
        orders_.clear();
        day_ = today;
    }
    return today;
}

void OrderStore::update_feed(const std::optional<std::string>& state, const std::optional<std::string>& detail) {
    std::lock_guard lock(mutex_);
    if (state) feed_state_ = *state;
    if (detail) feed_detail_ = *detail;
}

void OrderStore::received() {
    std::lock_guard lock(mutex_);
    last_message_at_ = iso_utc_offset(now_());
}

json OrderStore::feed_status() {
    std::lock_guard lock(mutex_);
    json feed;
    feed["state"] = feed_state_;
    feed["last_message_at"] = last_message_at_ ? json(*last_message_at_) : json(nullptr);
    feed["invalid_messages"] = invalid_messages_;
    feed["ignored_messages"] = ignored_messages_;
    feed["detail"] = feed_detail_;
    return feed;
}

void OrderStore::reject_message() {
    std::lock_guard lock(mutex_);
    ++invalid_messages_;
}

bool OrderStore::upsert(const orders::OrderEvent& event) {
    const json& data = event.payload;
    std::lock_guard lock(mutex_);
    std::string today = rollover();
    const AccountMeta* meta = accounts_.find(data.at("account_id").get<std::string>());
    if (!meta) {
        // Unknown or hidden account IDs in Kafka are ignored.
        ++ignored_messages_;
        return false;
    }
    if (const json* num = json_find(data, "account_num_id")) {
        bool matches = num->is_number() && num->get<double>() == static_cast<double>(meta->num_id);
        if (!matches) throw std::invalid_argument("account_num_id does not match the configured account_id.");
    }
    std::string event_day = zone_.date(event.created_at);
    if (reporting_ && event_day <= today) reporting_(event);
    if (event_day != today) {
        ++ignored_messages_;
        return false;
    }
    auto key = std::make_pair(meta->id, data.at("order_id").get<std::string>());
    auto previous = orders_.find(key);
    if (previous != orders_.end()) {
        if (sequence_of(data) <= sequence_of(previous->second.payload)) return false;
        if (event.created_at != previous->second.created_at) throw std::invalid_argument("created_at is immutable for an order.");
        if (event.updated_at < previous->second.updated_at) {
            throw std::invalid_argument("updated_at cannot decrease when sequence increases.");
        }
        previous->second = event;
    } else {
        orders_.emplace(key, event);
    }
    return true;
}

json OrderStore::snapshot() {
    std::lock_guard lock(mutex_);
    std::string today = rollover();
    std::vector<const orders::OrderEvent*> ordered;
    for (auto& [key, event] : orders_) ordered.push_back(&event);
    // Newest update first; ties fall back to account and order id, also descending.
    std::sort(ordered.begin(), ordered.end(), [](const orders::OrderEvent* a, const orders::OrderEvent* b) {
        if (a->updated_at != b->updated_at) return a->updated_at > b->updated_at;
        const std::string& account_a = a->payload.at("account_id").get_ref<const std::string&>();
        const std::string& account_b = b->payload.at("account_id").get_ref<const std::string&>();
        if (account_a != account_b) return account_a > account_b;
        return a->payload.at("order_id").get_ref<const std::string&>() > b->payload.at("order_id").get_ref<const std::string&>();
    });
    json rows = json::array();
    for (const orders::OrderEvent* event : ordered) {
        const AccountMeta* meta = accounts_.find(event->payload.at("account_id").get<std::string>());
        json row = event->payload;
        row["account_num_id"] = meta->num_id;
        row["broker"] = meta->broker;
        row["trading_medium"] = meta->trading_medium;
        rows.push_back(std::move(row));
    }
    json out;
    out["date"] = today;
    out["timezone"] = zone_.name();
    out["generated_at"] = iso_utc_offset(now_());
    out["orders"] = std::move(rows);
    json feed;
    feed["state"] = feed_state_;
    feed["last_message_at"] = last_message_at_ ? json(*last_message_at_) : json(nullptr);
    feed["invalid_messages"] = invalid_messages_;
    feed["ignored_messages"] = ignored_messages_;
    feed["detail"] = feed_detail_;
    out["feed"] = feed;
    return out;
}

}  // namespace ng::order_ui
