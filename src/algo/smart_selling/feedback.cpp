#include "algo/smart_selling/feedback.h"

#include <map>

#include "core/strings.h"

namespace ng::smart {

OrderEvent parse_smart_selling_snapshot(const std::string& raw) { return parse_order_message(raw); }

OrderEvent parse_smart_selling_snapshot(const json& payload) {
    std::string encoded = json_dump_compact(payload);
    if (encoded.size() > kMaxOrderMessageBytes) throw std::invalid_argument("Order snapshot exceeds 64 KiB.");
    return parse_order_payload(payload);
}

SmartSellingFeedback::SmartSellingFeedback(SmartSellingEngine& engine, int max_cached_orders, const std::string& state_path)
    : engine_(engine), max_cached_orders_(max_cached_orders), store_(state_path) {
    if (max_cached_orders < 1) throw std::invalid_argument("max_cached_orders must be positive");
}

void SmartSellingFeedback::track_external_order(const json& command) {
    std::lock_guard lock(mutex_);
    store_.track_external_order(command);
}

bool SmartSellingFeedback::has_open_orders(const std::string& account_id, const std::string& symbol) {
    std::lock_guard lock(mutex_);
    return store_.has_open_orders(account_id, symbol);
}

void SmartSellingFeedback::close() {
    std::lock_guard lock(mutex_);
    store_.close();
}

std::optional<json> SmartSellingFeedback::find_owned_context(const json& payload) {
    std::map<std::string, json> matches;
    for (const char* field : {"client_order_id", "command_id"}) {
        const json* identity = json_find(payload, field);
        if (!identity || !identity->is_string() || identity->get<std::string>().empty()) continue;
        auto context = engine_.get_order_context(identity->get<std::string>());
        if (context) matches[context->at("client_order_id").get<std::string>()] = *context;
    }
    auto broker_context =
        engine_.find_order_by_broker_id(payload.at("account_id").get<std::string>(), payload.at("order_id").get<std::string>());
    if (broker_context) matches[broker_context->at("client_order_id").get<std::string>()] = *broker_context;
    if (matches.size() > 1) {
        for (auto& [id, context] : matches) {
            hold_identified_conflict(context, payload, "conflicting order identifiers require reconciliation");
        }
        throw std::invalid_argument("Order snapshot identifiers refer to different Smart selling children");
    }
    if (matches.empty()) return std::nullopt;
    return matches.begin()->second;
}

void SmartSellingFeedback::validate_owned_snapshot(const json& payload, const json& context) {
    if (payload.at("account_id") != context.at("account_id")) {
        throw std::invalid_argument("Order snapshot account does not match the owned child");
    }
    if (canonical_order_symbol(payload.at("symbol").get<std::string>()) !=
        canonical_order_symbol(context.at("symbol").get<std::string>())) {
        throw std::invalid_argument("Order snapshot symbol does not match the owned child");
    }
    std::string owned_side = context.contains("side") ? context.at("side").get<std::string>() : std::string("SELL");
    if (payload.at("side").get<std::string>() != owned_side || payload.at("order_type").get<std::string>() != "LIMIT") {
        throw std::invalid_argument("Smart order feedback requires the owned side and ordinary limit");
    }
    const json& quantity = payload.at("quantity");
    Decimal expected_quantity = parse_decimal(context.at("quantity"));
    if (quantity.is_null() || parse_decimal(quantity) != expected_quantity) {
        throw std::invalid_argument("Order snapshot quantity does not match the original child");
    }
    if (parse_decimal(payload.at("limit_price")) != parse_decimal(context.at("limit_price"))) {
        throw std::invalid_argument("Order snapshot limit price does not match the fixed child price");
    }
    const json& broker = context.at("broker_order_id");
    if (!broker.is_null() && broker != payload.at("order_id")) {
        throw std::invalid_argument("Order snapshot broker ID does not match the owned child");
    }
}

void SmartSellingFeedback::hold_identified_conflict(const std::optional<json>& context, const json& payload,
                                                    const std::string& reason) {
    if (context && context->at("account_id") == payload.at("account_id")) {
        engine_.hold_order(context->at("client_order_id").get<std::string>(), reason);
    }
}

bool SmartSellingFeedback::older_report_involves_unknown(const json& payload, const std::optional<FeedbackRow>& previous) {
    if (!previous) return false;
    long long sequence = payload.at("sequence").is_number_float() ? static_cast<long long>(payload.at("sequence").get<double>())
                                                                  : payload.at("sequence").get<long long>();
    std::string status = payload.at("status").get<std::string>();
    return sequence < previous->sequence && (status == "UNKNOWN" || previous->status == "UNKNOWN");
}

bool SmartSellingFeedback::should_ignore_retained_report(const OrderEvent& event, const std::optional<FeedbackRow>& account_previous,
                                                         const OrderEvent* cached) {
    const json& payload = event.payload;
    if (older_report_involves_unknown(payload, account_previous)) return true;
    if (!cached) return false;
    if (event.created_at != cached->created_at) throw std::invalid_argument("Order snapshot changed the original creation time");
    long long seq = payload.at("sequence").get<long long>();
    long long cached_seq = cached->payload.at("sequence").get<long long>();
    if (seq > cached_seq && event.updated_at < cached->updated_at) {
        throw std::invalid_argument("Order snapshot source time decreased as sequence increased");
    }
    FeedbackRow cached_row;
    cached_row.sequence = cached_seq;
    cached_row.status = cached->payload.at("status").get<std::string>();
    return older_report_involves_unknown(payload, cached_row);
}

void SmartSellingFeedback::apply_cumulative_report(const std::string& client_id, const json& payload) {
    std::string status = payload.at("status").get<std::string>();
    bool authoritative = status != "UNKNOWN";
    OrderReport report;
    report.status = status;
    report.cumulative_filled = payload.at("filled_quantity");
    report.broker_order_id = payload.at("order_id").get<std::string>();
    report.sequence = payload.at("sequence").get<long long>();
    report.authoritative = authoritative;
    report.final = authoritative && is_feedback_terminal(status);
    engine_.on_order_report(client_id, report);
}

void SmartSellingFeedback::remember_snapshot(const std::string& client_id, const OrderEvent& event) {
    auto it = latest_.find(client_id);
    if (it == latest_.end()) {
        lru_.push_back(client_id);
        latest_.emplace(client_id, std::make_pair(event, std::prev(lru_.end())));
    } else {
        if (event.payload.at("sequence").get<long long>() > it->second.first.payload.at("sequence").get<long long>()) {
            it->second.first = event;
        }
        lru_.erase(it->second.second);
        lru_.push_back(client_id);
        it->second.second = std::prev(lru_.end());
    }
    while (static_cast<int>(latest_.size()) > max_cached_orders_) {
        latest_.erase(lru_.front());
        lru_.pop_front();
    }
}

bool SmartSellingFeedback::apply_snapshot(const std::string& raw) { return apply_snapshot(parse_smart_selling_snapshot(raw)); }

bool SmartSellingFeedback::apply_snapshot(const OrderEvent& event) {
    const json& payload = event.payload;
    std::lock_guard lock(mutex_);
    std::optional<json> context = find_owned_context(payload);
    std::optional<FeedbackRow> account_previous;
    try {
        if (context) validate_owned_snapshot(payload, *context);
        account_previous = store_.record_snapshot(event);
    } catch (const std::invalid_argument&) {
        hold_identified_conflict(context, payload, "conflicting order metadata requires reconciliation");
        throw;
    }
    if (!context) return false;
    std::string client_id = context->at("client_order_id").get<std::string>();
    auto cached_it = latest_.find(client_id);
    const OrderEvent* cached = cached_it == latest_.end() ? nullptr : &cached_it->second.first;
    if (should_ignore_retained_report(event, account_previous, cached)) return true;
    apply_cumulative_report(client_id, payload);
    remember_snapshot(client_id, event);
    return true;
}

bool SmartSellingFeedback::reconcile(const std::string& client_order_id) {
    std::lock_guard lock(mutex_);
    auto it = latest_.find(client_order_id);
    if (it == latest_.end()) return false;
    OrderEvent event = it->second.first;
    return apply_snapshot(event);
}

std::optional<json> SmartSellingFeedback::latest_snapshot(const std::string& client_order_id) {
    std::lock_guard lock(mutex_);
    auto it = latest_.find(client_order_id);
    if (it == latest_.end()) return std::nullopt;
    return it->second.first.payload;
}

}  // namespace ng::smart
