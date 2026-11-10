#include "algo/smart_selling/market_adapter.h"

#include <algorithm>
#include <cmath>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng::smart {

std::optional<Decimal> market_decimal(const json* value) {
    if (!value || value->is_null() || value->is_boolean()) return std::nullopt;
    if (value->is_number_integer()) return Decimal(value->get<long long>());
    if (value->is_number_unsigned()) return Decimal::try_parse(std::to_string(value->get<unsigned long long>()));
    if (value->is_number_float()) {
        double d = value->get<double>();
        if (!std::isfinite(d)) return std::nullopt;
        return Decimal::from_double(d);
    }
    if (value->is_string()) return Decimal::try_parse(value->get<std::string>());
    return std::nullopt;
}

std::optional<double> market_timestamp(const json* value, bool nanoseconds) {
    if (!value || value->is_null()) return std::nullopt;
    auto number = market_decimal(value);
    if (number) {
        if (nanoseconds) return number->div(Decimal(1000000000), 9).to_double();
        return number->to_double();
    }
    if (value->is_string()) {
        auto parsed = parse_iso8601(value->get<std::string>());
        if (parsed && parsed->has_offset) return parsed->epoch_seconds;
    }
    return std::nullopt;
}

std::optional<long long> market_sequence(const json* value) {
    auto number = market_decimal(value);
    if (!number || number->is_negative() || !number->is_integral()) return std::nullopt;
    try {
        return number->to_int64();
    } catch (const DecimalError&) {
        return std::nullopt;
    }
}

std::pair<std::optional<Decimal>, Decimal> best_level(const json* side, bool bid) {
    if (!side || !side->is_object()) return {std::nullopt, Decimal::zero()};
    const json* levels = json_find(*side, "levels");
    if (!levels || !levels->is_array()) return {std::nullopt, Decimal::zero()};
    std::vector<std::pair<Decimal, std::optional<Decimal>>> parsed;
    for (const auto& level : *levels) {
        if (!level.is_object()) continue;
        auto price = market_decimal(json_first_present(level, "prc", "price", "Price"));
        auto quantity = market_decimal(json_first_present(level, "qty", "total_qty", "quantity", "TotalSize"));
        if (price && price->is_positive()) parsed.emplace_back(*price, quantity);
    }
    if (parsed.empty()) return {std::nullopt, Decimal::zero()};
    Decimal best = parsed[0].first;
    for (auto& [price, qty] : parsed) best = bid ? Decimal::max(best, price) : Decimal::min(best, price);
    Decimal total;
    for (auto& [price, qty] : parsed) {
        if (price != best) continue;
        if (!qty || qty->is_negative()) return {best, Decimal::zero()};
        total += *qty;
    }
    return {best, total};
}

std::vector<Decimal> displayed_prices(const json* side) {
    std::vector<Decimal> prices;
    if (!side || !side->is_object()) return prices;
    const json* levels = json_find(*side, "levels");
    if (!levels || !levels->is_array()) return prices;
    for (const auto& level : *levels) {
        if (!level.is_object()) continue;
        auto price = market_decimal(json_first_present(level, "prc", "price", "Price"));
        auto quantity = market_decimal(json_first_present(level, "qty", "total_qty", "quantity", "TotalSize"));
        if (price && price->is_positive() && quantity && quantity->is_positive()) prices.push_back(*price);
    }
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
    return prices;
}

SmartSellingMarketAdapter::SmartSellingMarketAdapter(SmartSellingEngine& engine, Clock clock, Clock wall_clock,
                                                     double max_source_age, double rate_window_seconds, int max_observations,
                                                     double resume_cooldown_seconds)
    : engine_(engine),
      clock_(clock ? std::move(clock) : Clock(&mono_now)),
      wall_clock_(wall_clock ? std::move(wall_clock) : Clock(&wall_now)),
      max_source_age_(max_source_age),
      rate_window_seconds_(rate_window_seconds),
      max_observations_(std::max(2, max_observations)),
      resume_cooldown_seconds_(std::max(0.0, resume_cooldown_seconds)) {}

bool SmartSellingMarketAdapter::fresh(const std::optional<double>& timestamp) const {
    if (!timestamp) return false;
    double age = wall_clock_() - *timestamp;
    return -1 <= age && age <= max_source_age_;
}

SmartSellingMarketAdapter::SymbolState& SmartSellingMarketAdapter::symbol_state(const std::string& symbol) {
    auto it = symbols_.find(symbol);
    if (it != symbols_.end()) return it->second;
    SymbolState state;
    auto checkpoint = engine_.get_market_checkpoint(symbol);
    if (checkpoint && checkpoint->is_object()) {
        state.credited_session = json_to_string(json_find(*checkpoint, "session")).value_or("");
        if (const json* s = json_find(*checkpoint, "session"); !s || s->is_null()) state.credited_session = "";
        state.credited_cumulative = market_decimal(json_find(*checkpoint, "cumulative"));
        state.credited_at = market_timestamp(json_find(*checkpoint, "observed_at"));
        const json* conflict = json_find(*checkpoint, "counter_conflict");
        state.counter_conflict = conflict && conflict->is_boolean() && conflict->get<bool>();
        const json* halted = json_find(*checkpoint, "halted");
        if (halted && halted->is_boolean() && halted->get<bool>()) {
            state.halt_latched = true;
            state.market_state = "HALTED";
            state.market_reason = "persisted halt requires fresh reopening checks";
        }
    }
    return symbols_[symbol] = state;
}

void SmartSellingMarketAdapter::save_checkpoint(const std::string& symbol, SymbolState& state) {
    json checkpoint;
    checkpoint["session"] = state.credited_session;
    checkpoint["cumulative"] = state.credited_cumulative ? json(state.credited_cumulative->str()) : json(nullptr);
    checkpoint["observed_at"] = state.credited_at ? json(*state.credited_at) : json(nullptr);
    checkpoint["halted"] = state.halt_latched;
    checkpoint["counter_conflict"] = state.counter_conflict;
    engine_.set_market_checkpoint(symbol, checkpoint);
}

void SmartSellingMarketAdapter::reset_volume(SymbolState& state) {
    state.volume.reset();
    state.volume_time.reset();
    state.volume_sequence.reset();
    state.observations.clear();
}

void SmartSellingMarketAdapter::set_connected(bool connected) {
    std::lock_guard lock(mutex_);
    if (connected == connected_) return;
    connected_ = connected;
    std::string reason = connected ? "waiting for fresh feed snapshot" : "feed disconnected";
    for (auto& [symbol, state] : symbols_) {
        state.reset_quote = true;
        state.quote_healthy = false;
        engine_.set_feed_health(symbol, false, reason);
    }
}

void SmartSellingMarketAdapter::set_volume_connected(bool connected) {
    std::lock_guard lock(mutex_);
    if (connected == volume_connected_) return;
    volume_connected_ = connected;
    volume_connected_since_ = connected ? std::optional<double>(wall_clock_()) : std::nullopt;
    std::string reason = connected ? "waiting for fresh trading-volume baseline" : "trading-volume feed disconnected";
    for (auto& [symbol, state] : symbols_) {
        reset_volume(state);
        state.volume_healthy = false;
        state.resume_count = 0;
        if (state.halt_latched) {
            state.market_state = "HALTED";
            state.market_reason = "halt status requires fresh confirmation";
            state.resume_started.reset();
            state.resume_source.reset();
        } else {
            state.market_state = "UNKNOWN";
            state.market_reason = "trading-volume feed disconnected";
        }
        engine_.set_feed_health(symbol, false, reason);
    }
}

bool SmartSellingMarketAdapter::volume_is_healthy(const SymbolState& state) const {
    return volume_connected_ && state.volume_healthy && fresh(state.volume_time) && fresh(state.status_time) &&
           state.market_state == "TRADING" && !state.counter_conflict;
}

void SmartSellingMarketAdapter::refresh_health(const std::string& symbol, SymbolState& state) {
    bool healthy = connected_ && state.quote_healthy && fresh(state.book_time) && volume_is_healthy(state);
    std::string reason =
        state.market_state != "TRADING" ? state.market_reason : std::string("trading-volume or quote feed unavailable");
    engine_.set_feed_health(symbol, healthy, healthy ? "" : reason);
}

MarketStatus SmartSellingMarketAdapter::market_status(const std::string& symbol_in) {
    std::lock_guard lock(mutex_);
    SymbolState& state = symbol_state(to_upper(trim(symbol_in)));
    if (state.counter_conflict && state.market_state != "HALTED") {
        return {"UNKNOWN", "cumulative volume correction requires reconciliation"};
    }
    if (state.market_state == "TRADING" && !volume_is_healthy(state)) {
        return {"UNKNOWN", "trading-volume status is stale or unavailable"};
    }
    return {state.market_state, state.market_reason};
}

bool SmartSellingMarketAdapter::halt_gate(const std::string& symbol, SymbolState& state, const json& payload, double source) {
    const json* halted = json_find(payload, "is_halted");
    const json* delayed = json_find(payload, "is_delayed");
    const json* healthy = json_find(payload, "healthy");
    bool halted_true = halted && halted->is_boolean() && halted->get<bool>();
    bool halted_false = halted && halted->is_boolean() && !halted->get<bool>();
    bool delayed_false = delayed && delayed->is_boolean() && !delayed->get<bool>();
    bool healthy_true = !healthy || (healthy->is_boolean() && healthy->get<bool>());
    if (halted_true) {
        state.halt_latched = true;
        state.market_state = "HALTED";
        state.market_reason = "exchange halt reported";
        state.resume_started.reset();
        state.resume_source.reset();
        state.resume_count = 0;
        save_checkpoint(symbol, state);
        return false;
    }
    if (!halted_false || !delayed_false || !healthy_true) {
        state.resume_count = 0;
        if (!state.halt_latched) state.market_state = "UNKNOWN";
        state.market_reason = "awaiting explicit nonhalted, real-time healthy status";
        return false;
    }
    if (state.halt_latched) {
        if (!state.resume_started) {
            state.resume_started = clock_();
            state.resume_source = source;
            state.resume_count = 0;
        }
        state.resume_count += 1;
        state.market_state = "RESUMING";
        state.market_reason = "discarding reopening volume; awaiting stable trading and a fresh book";
        bool refreshed_book = state.quote_healthy && fresh(state.book_time) && state.book_snapshot_time &&
                              state.resume_source && *state.book_snapshot_time > *state.resume_source &&
                              fresh(state.book_snapshot_time);
        bool cooldown_complete = clock_() - *state.resume_started >= resume_cooldown_seconds_;
        if (state.resume_count >= 2 && cooldown_complete && refreshed_book && connected_) {
            state.halt_latched = false;
            state.market_state = "TRADING";
            state.market_reason = "reopening checks complete; new volume baseline established";
            save_checkpoint(symbol, state);
        }
        return false;
    }
    if (state.market_state != "TRADING") reset_volume(state);
    state.market_state = "TRADING";
    state.market_reason = "explicit nonhalted, real-time status confirmed";
    return true;
}

bool SmartSellingMarketAdapter::accept_offset(const std::optional<std::string>& topic, std::optional<int> partition,
                                              std::optional<long long> offset) {
    if (!topic || !partition || !offset) return true;
    auto key = std::make_pair(*topic, *partition);
    auto it = offsets_.find(key);
    if (it != offsets_.end() && *offset <= it->second) return false;
    offsets_[key] = *offset;
    return true;
}

void SmartSellingMarketAdapter::observation(SymbolState& state, double start, double end, const Decimal& quantity) {
    if (end > start) state.observations.emplace_back(start, end, quantity);
    while (!state.observations.empty() &&
           (static_cast<int>(state.observations.size()) > max_observations_ ||
            std::get<1>(state.observations.front()) < wall_clock_() - rate_window_seconds_)) {
        state.observations.pop_front();
    }
}

std::optional<Decimal> SmartSellingMarketAdapter::volume_rate(const std::string& symbol_in) {
    std::lock_guard lock(mutex_);
    auto it = symbols_.find(to_upper(trim(symbol_in)));
    if (it == symbols_.end()) return std::nullopt;
    SymbolState& state = it->second;
    if (!volume_is_healthy(state) || state.observations.empty()) return std::nullopt;
    double now = wall_clock_();
    if (!fresh(std::get<1>(state.observations.back()))) return std::nullopt;
    double cutoff = now - rate_window_seconds_;
    while (!state.observations.empty() && std::get<1>(state.observations.front()) <= cutoff) state.observations.pop_front();
    if (state.observations.empty()) return std::nullopt;
    double start = std::max(std::get<0>(state.observations.front()), cutoff);
    Decimal elapsed = Decimal::from_double(now - start);
    if (elapsed <= Decimal::zero()) return std::nullopt;
    Decimal volume;
    for (auto& [interval_start, interval_end, quantity] : state.observations) {
        Decimal covered = Decimal::from_double(interval_end - std::max(interval_start, cutoff));
        Decimal duration = Decimal::from_double(interval_end - interval_start);
        if (duration.is_zero()) continue;
        Decimal fraction = covered.div(duration, 30);
        volume += quantity * fraction;
    }
    return volume.div(elapsed, 30);
}

bool SmartSellingMarketAdapter::apply_price_book(const json& payload, const std::optional<std::string>& topic,
                                                 std::optional<int> partition, std::optional<long long> offset) {
    if (!payload.is_object()) return false;
    std::string symbol = to_upper(trim(json_to_string(json_find(payload, "symbol")).value_or("")));
    if (symbol.empty()) return false;
    std::lock_guard lock(mutex_);
    if (!connected_) return false;
    if (!accept_offset(topic, partition, offset)) return false;
    SymbolState& state = symbol_state(symbol);
    std::optional<double> book_time = market_timestamp(json_find(payload, "book_observed_at"));
    if (!book_time) book_time = market_timestamp(json_find(payload, "update_time"), true);
    std::string feed_id = json_to_string(json_find(payload, "feed_id")).value_or("legacy");
    if (const json* f = json_find(payload, "feed_id"); !f || f->is_null() || (f->is_string() && f->get<std::string>().empty())) {
        feed_id = "legacy";
    }
    bool new_feed = !state.book_feed_id.empty() && state.book_feed_id != feed_id;
    state.book_feed_id = feed_id;
    std::optional<long long> strict_sequence = market_sequence(json_first_present(payload, "book_sequence", "provider_sequence"));
    std::optional<long long> raw_sequence = market_sequence(json_find(payload, "sequence_num"));
    if (new_feed) {
        state.book_time.reset();
        state.book_sequence.reset();
        state.raw_sequence.reset();
        state.reset_quote = true;
    }
    if (book_time && state.book_time && *book_time < *state.book_time) return false;
    if (strict_sequence && state.book_sequence && !state.reset_quote) {
        if (*strict_sequence <= *state.book_sequence) return false;
    }
    if (!strict_sequence && raw_sequence && state.raw_sequence) {
        if (*raw_sequence == *state.raw_sequence) return false;
        if (*raw_sequence < *state.raw_sequence) {
            if (!book_time || !state.book_time || *book_time <= *state.book_time) return false;
            state.reset_quote = true;
        }
    }
    bool gap = strict_sequence && state.book_sequence && !state.reset_quote && *strict_sequence != *state.book_sequence + 1;
    bool recovering = state.reset_quote || state.quote_gap;
    auto [bid, bid_size] = best_level(json_find(payload, "bid_side"), true);
    auto [ask, ask_size] = best_level(json_find(payload, "offer_side"), false);
    const json* healthy_flag = json_find(payload, "healthy");
    bool payload_healthy = !(healthy_flag && healthy_flag->is_boolean() && !healthy_flag->get<bool>());
    bool quote_healthy = fresh(book_time) && !gap && payload_healthy && bid && ask && bid_size.is_positive() && *ask >= *bid;
    const json* accessible_flag = json_find(payload, "accessible");
    bool accessible = !(accessible_flag && accessible_flag->is_boolean() && !accessible_flag->get<bool>());
    state.quote_healthy = quote_healthy && accessible;
    const json* kind = json_find(payload, "kind");
    bool heartbeat = kind && kind->is_string() && kind->get<std::string>() == "PRICE_BOOK_HEARTBEAT";
    QuoteUpdate update;
    update.bid = bid ? json(bid->str()) : json(nullptr);
    update.ask = ask ? json(ask->str()) : json(nullptr);
    update.bid_size = json(bid_size.str());
    update.ask_size = json(ask_size.str());
    auto level_list = [](const std::vector<Decimal>& prices) {
        std::vector<json> out;
        for (const Decimal& price : prices) out.push_back(json(price.str()));
        return out;
    };
    update.bid_prices = level_list(displayed_prices(json_find(payload, "bid_side")));
    update.ask_prices = level_list(displayed_prices(json_find(payload, "offer_side")));
    update.source_time = book_time ? json(*book_time) : json(nullptr);
    update.sequence = strict_sequence;
    update.healthy = state.quote_healthy && volume_is_healthy(state);
    update.accessible = accessible;
    update.received_at = clock_();
    update.reset_sequence = recovering && !gap;
    update.preserve_consumption = heartbeat;
    engine_.on_quote(symbol, update);
    if (book_time) state.book_time = book_time;
    if (quote_healthy && !heartbeat) state.book_snapshot_time = book_time;
    state.book_sequence = strict_sequence;
    state.raw_sequence = raw_sequence;
    state.quote_gap = gap;
    state.reset_quote = false;
    return true;
}

bool SmartSellingMarketAdapter::apply_volume(const json& payload, const std::optional<std::string>& topic,
                                             std::optional<int> partition, std::optional<long long> offset) {
    if (!payload.is_object()) return false;
    const json* type = json_find(payload, "type");
    if (type && !(type->is_string() && type->get<std::string>() == "TRADING_VOLUME")) return false;
    const json* version = json_find(payload, "schema_version");
    if (version && !((version->is_number_integer() && version->get<long long>() == 1) ||
                     (version->is_number_float() && version->get<double>() == 1.0))) {
        return false;
    }
    std::string symbol = to_upper(trim(json_to_string(json_find(payload, "symbol")).value_or("")));
    if (symbol.empty()) return false;
    std::lock_guard lock(mutex_);
    if (!volume_connected_ || !accept_offset(topic, partition, offset)) return false;
    SymbolState& state = symbol_state(symbol);
    auto envelope = parse_volume_envelope(symbol, payload);
    if (!envelope) {
        reset_volume(state);
        state.volume_healthy = false;
        state.resume_count = 0;
        refresh_health(symbol, state);
        return false;
    }
    if (!accept_volume_status(state, *envelope, payload)) return false;
    check_credited_watermark(state, *envelope);
    if (!prepare_volume_baseline(state, *envelope, payload)) return false;
    bool admit = halt_gate(symbol, state, payload, envelope->observed_at);
    bool accepted;
    if (admit && !state.counter_conflict) {
        admit_volume(state, *envelope, payload);
        accepted = true;
    } else {
        update_blocked_baseline(state, *envelope, payload);
        const json* healthy = json_find(payload, "healthy");
        accepted = !healthy || (healthy->is_boolean() && healthy->get<bool>());
    }
    state.volume_healthy = state.volume_time.has_value();
    refresh_health(symbol, state);
    return accepted;
}

std::optional<SmartSellingMarketAdapter::VolumeEnvelope> SmartSellingMarketAdapter::parse_volume_envelope(
    const std::string& symbol, const json& payload) {
    std::optional<double> source = market_timestamp(json_find(payload, "observed_at"));
    std::optional<long long> sequence = market_sequence(json_find(payload, "sequence"));
    std::string feed_id = json_to_string(json_find(payload, "feed_id")).value_or("");
    if (const json* f = json_find(payload, "feed_id"); f && f->is_null()) feed_id = "";
    if (feed_id.empty() || !sequence || !fresh(source)) return std::nullopt;
    std::string session;
    const json* raw_session = json_find(payload, "session");
    if (raw_session && !raw_session->is_null() && !(raw_session->is_string() && raw_session->get<std::string>().empty())) {
        session = json_to_string(raw_session).value_or("");
    } else {
        session = iso_utc(*source).substr(0, 10);
    }
    VolumeEnvelope env;
    env.symbol = symbol;
    env.feed_id = feed_id;
    env.sequence = *sequence;
    env.observed_at = *source;
    env.session = session;
    env.total_volume = market_decimal(json_find(payload, "total_volume"));
    return env;
}

bool SmartSellingMarketAdapter::accept_volume_status(SymbolState& state, const VolumeEnvelope& envelope, const json& payload) {
    double source = envelope.observed_at;
    const json* halted = json_find(payload, "is_halted");
    bool fresh_halt = halted && halted->is_boolean() && halted->get<bool>();
    if (volume_connected_since_ && source < *volume_connected_since_) return false;
    if (state.status_time) {
        if (source < *state.status_time || (source == *state.status_time && !fresh_halt)) return false;
    }
    if (state.status_feed_id == envelope.feed_id && state.status_sequence) {
        if (envelope.sequence <= *state.status_sequence) return false;
    }
    state.status_feed_id = envelope.feed_id;
    state.status_sequence = envelope.sequence;
    state.status_time = source;
    if (fresh_halt) {
        halt_gate(envelope.symbol, state, payload, source);
        refresh_health(envelope.symbol, state);
    }
    return true;
}

void SmartSellingMarketAdapter::check_credited_watermark(SymbolState& state, const VolumeEnvelope& envelope) {
    if (envelope.session != state.credited_session || !envelope.total_volume) return;
    if (!state.credited_cumulative || *envelope.total_volume >= *state.credited_cumulative) return;
    state.counter_conflict = true;
    save_checkpoint(envelope.symbol, state);
    state.volume_feed_id = envelope.feed_id;
    unknown_correction(envelope.symbol, state, *envelope.total_volume - *state.credited_cumulative, envelope.observed_at,
                       envelope.sequence);
}

bool SmartSellingMarketAdapter::prepare_volume_baseline(SymbolState& state, const VolumeEnvelope& envelope, const json& payload) {
    if (state.volume_feed_id != envelope.feed_id || state.session != envelope.session) {
        reset_volume(state);
        state.resume_count = 0;
    }
    state.volume_feed_id = envelope.feed_id;
    state.session = envelope.session;
    if (state.volume_sequence && envelope.sequence <= *state.volume_sequence) return false;
    if (state.volume_time && envelope.observed_at <= *state.volume_time) return false;
    bool sequence_gap = state.volume_sequence && envelope.sequence != *state.volume_sequence + 1;
    bool time_gap = state.volume_time && envelope.observed_at - *state.volume_time > max_source_age_;
    if (sequence_gap || time_gap) {
        const json* trades_field = json_find(payload, "trades");
        bool has_trades = trades_field && trades_field->is_array();
        if (!has_trades && envelope.total_volume && state.volume && *envelope.total_volume < *state.volume) {
            unknown_correction(envelope.symbol, state, *envelope.total_volume - *state.volume, envelope.observed_at,
                               envelope.sequence);
        }
        reset_volume(state);
        state.resume_count = 0;
    }
    return true;
}

void SmartSellingMarketAdapter::update_blocked_baseline(SymbolState& state, const VolumeEnvelope& envelope, const json& payload) {
    const std::optional<Decimal>& current = envelope.total_volume;
    if (state.volume && current && *current < *state.volume) {
        unknown_correction(envelope.symbol, state, *current - *state.volume, envelope.observed_at, envelope.sequence);
    }
    reset_volume(state);
    const json* trades_field = json_find(payload, "trades");
    if (trades_field && trades_field->is_array()) {
        state.volume_mode = "trades";
        trades(envelope.symbol, state, *trades_field, true);
    } else {
        state.volume_mode = "cumulative";
    }
    const json* healthy = json_find(payload, "healthy");
    bool healthy_true = !healthy || (healthy->is_boolean() && healthy->get<bool>());
    if (healthy_true && current && !current->is_negative()) {
        state.volume = current;
        state.volume_time = envelope.observed_at;
        state.volume_sequence = envelope.sequence;
    }
}

void SmartSellingMarketAdapter::admit_volume(SymbolState& state, const VolumeEnvelope& envelope, const json& payload) {
    const json* trades_field = json_find(payload, "trades");
    std::string mode = (trades_field && trades_field->is_array()) ? "trades" : "cumulative";
    if (state.volume_mode != mode) reset_volume(state);
    state.volume_mode = mode;
    if (mode == "trades") {
        trades(envelope.symbol, state, *trades_field, false);
        state.volume_time = envelope.observed_at;
        state.volume_sequence = envelope.sequence;
    } else {
        cumulative(envelope.symbol, state, payload, envelope.observed_at, envelope.sequence);
    }
}

void SmartSellingMarketAdapter::cumulative(const std::string& symbol, SymbolState& state, const json& payload, double source,
                                           long long sequence) {
    auto volume = market_decimal(json_find(payload, "total_volume"));
    if (!volume || volume->is_negative()) {
        reset_volume(state);
        return;
    }
    std::optional<Decimal> previous = state.volume;
    std::optional<double> previous_time = state.volume_time;
    state.volume = volume;
    state.volume_time = source;
    state.volume_sequence = sequence;
    if (!previous) return;
    Decimal delta = *volume - *previous;
    if (delta.is_negative()) {
        unknown_correction(symbol, state, delta, source, sequence);
        state.observations.clear();
        return;
    }
    observation(state, *previous_time, source, delta);
    if (delta.is_zero()) return;
    std::optional<double> interval_start = market_timestamp(json_find(payload, "interval_start"));
    double start = interval_start ? std::min(*previous_time, *interval_start) : *previous_time;
    state.credited_session = state.session;
    state.credited_cumulative = volume;
    state.credited_at = source;
    save_checkpoint(symbol, state);
    TradeUpdate trade;
    trade.quantity = json(delta.str());
    trade.event_id = event_id(symbol, state, sequence);
    trade.source_time = json(source);
    trade.interval_start = json(start);
    trade.received_at = clock_();
    engine_.on_trade(symbol, trade);
}

std::string SmartSellingMarketAdapter::event_id(const std::string& symbol, const SymbolState& state,
                                                const std::optional<long long>& sequence) {
    return state.volume_feed_id + ":" + symbol + ":" + (sequence ? std::to_string(*sequence) : "None");
}

void SmartSellingMarketAdapter::unknown_correction(const std::string& symbol, SymbolState& state, const Decimal& delta,
                                                   double source, const std::optional<long long>& sequence) {
    std::string id = event_id(symbol, state, sequence);
    TradeUpdate trade;
    trade.quantity = json(delta.str());
    trade.event_id = id;
    trade.source_time = json(source);
    trade.correction_of = "unreconstructable:" + id;
    trade.received_at = clock_();
    engine_.on_trade(symbol, trade);
}

void SmartSellingMarketAdapter::trades(const std::string& symbol, SymbolState& state, const json& trade_list,
                                       bool corrections_only) {
    int index = -1;
    for (const auto& trade : trade_list) {
        ++index;
        if (!trade.is_object()) continue;
        std::string event = json_to_string(json_first_present(trade, "event_id", "trade_id")).value_or("");
        if (const json* e = json_first_present(trade, "event_id", "trade_id"); e && e->is_null()) event = "";
        auto quantity = market_decimal(json_find(trade, "quantity"));
        auto source = market_timestamp(json_find(trade, "source_time"));
        const json* correction = json_find(trade, "correction_of");
        bool has_correction = correction && !correction->is_null();
        std::string correction_text = has_correction ? json_to_string(correction).value_or("") : "";
        bool incomplete_reference = has_correction && (event.empty() || !quantity || correction_text.empty());
        bool unreferenced_negative = quantity && quantity->is_negative() && !has_correction;
        if (incomplete_reference || unreferenced_negative) {
            std::string identifier = event_id(symbol, state, state.status_sequence) + ":invalid:" + std::to_string(index);
            TradeUpdate t;
            t.quantity = json("0");
            t.event_id = identifier;
            t.source_time = source ? json(*source) : json(nullptr);
            t.correction_of = "unreconstructable:" + identifier;
            t.received_at = clock_();
            engine_.on_trade(symbol, t);
            continue;
        }
        if (event.empty() || !quantity || (!has_correction && *quantity <= Decimal::zero())) continue;
        if (corrections_only && !has_correction) continue;
        if (!has_correction && (!fresh(source) || (volume_connected_since_ && source && *source < *volume_connected_since_))) continue;
        if (state.seen_trade_index.count(event)) continue;
        TradeUpdate t;
        t.quantity = json(quantity->str());
        t.event_id = event;
        t.source_time = source ? json(*source) : json(nullptr);
        t.sequence = market_sequence(json_find(trade, "sequence"));
        if (has_correction) t.correction_of = correction_text;
        t.received_at = clock_();
        engine_.on_trade(symbol, t);
        state.seen_trade_index[event] = true;
        state.seen_trades.push_back(event);
        while (static_cast<int>(state.seen_trades.size()) > max_observations_ * 4) {
            state.seen_trade_index.erase(state.seen_trades.front());
            state.seen_trades.pop_front();
        }
        if (has_correction) {
            state.observations.clear();
        } else if (state.volume_time && source && *source >= *state.volume_time) {
            observation(state, *state.volume_time, *source, *quantity);
        }
        if (source) state.volume_time = source;
    }
}

}  // namespace ng::smart
