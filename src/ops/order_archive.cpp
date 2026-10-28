#include "ops/order_archive.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

#include "algo/smart_selling/order_status.h"
#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "kafka/kafka_client.h"

namespace ng::ops {

namespace {

constexpr const char* kLog = "nextgen-gui.order-archive";

struct Candidate {
    json payload;
    double created_at = 0.0;
    double updated_at = 0.0;
    long long sequence = 0;
    long long timestamp_ms = -1;
    long long offset = 0;
    int partition = 0;
    std::size_t snapshot_index = 0;
};

bool newer(const Candidate& a, const Candidate& b) {
    if (a.sequence != b.sequence) return a.sequence > b.sequence;
    if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
    if (a.timestamp_ms != b.timestamp_ms) return a.timestamp_ms > b.timestamp_ms;
    return a.offset > b.offset;
}

void write_atomic(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << text;
        if (!out) throw std::runtime_error("write failed for " + tmp.string());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("cannot rename " + tmp.string() + " to " + path.string() + ": " + ec.message());
}

}  // namespace

json DayArchive::metadata() const {
    json out;
    out["date"] = date;
    out["timezone"] = timezone;
    out["source"] = "KTrader order-status retained Kafka snapshots";
    out["topic"] = topic;
    out["retrieved_at"] = iso_utc(retrieved_at);
    out["bounds"] = bounds;
    out["messages_read"] = messages_read;
    out["matching_updates"] = matching_updates;
    out["order_count"] = static_cast<long long>(orders.size());
    out["configured_accounts"] = configured_accounts;
    out["invalid"] = invalid;
    out["tombstones"] = tombstones;
    out["conflicts"] = conflicts;
    out["generator"] = std::string("nextgen-gui ") + NG_VERSION;
    return out;
}

DayArchive build_day_archive(const std::string& date, const std::vector<RawMessage>& messages, const AccountDirectory& accounts,
                             const TimeZone& zone, const std::string& topic, double retrieved_at) {
    DayArchive archive;
    archive.date = date;
    archive.timezone = zone.name();
    archive.retrieved_at = retrieved_at;
    archive.topic = topic;
    for (const AccountMeta& meta : accounts.ordered()) {
        archive.configured_accounts[meta.id] = json{{"broker", meta.broker}, {"num_id", meta.num_id}, {"trading_medium", meta.trading_medium}};
    }
    const std::string suffix = zone.field_suffix();
    std::map<int, std::pair<long long, long long>> bounds;
    std::map<std::pair<std::string, std::string>, Candidate> latest;
    std::map<std::pair<std::string, std::string>, std::map<long long, std::pair<std::string, std::vector<long long>>>> by_sequence;

    for (const RawMessage& message : messages) {
        ++archive.messages_read;
        auto b = bounds.find(message.partition);
        if (b == bounds.end()) bounds[message.partition] = {message.offset, message.offset};
        else {
            b->second.first = std::min(b->second.first, message.offset);
            b->second.second = std::max(b->second.second, message.offset);
        }
        if (message.tombstone || message.value.empty()) {
            archive.tombstones.push_back(json{{"partition", message.partition}, {"offset", message.offset}});
            continue;
        }
        smart::OrderEvent event;
        try {
            event = smart::parse_order_message(message.value);
        } catch (const std::exception& e) {
            archive.invalid.push_back(json{{"partition", message.partition}, {"offset", message.offset}, {"error", e.what()}});
            continue;
        }
        if (zone.date(event.created_at) != date) continue;
        ++archive.matching_updates;
        std::string account_id = event.payload.value("account_id", "");
        std::string order_id = event.payload.value("order_id", "");
        long long sequence = event.payload.value("sequence", 0LL);

        json snapshot;
        snapshot["topic"] = message.topic;
        snapshot["partition"] = message.partition;
        snapshot["offset"] = message.offset;
        snapshot["timestamp_ms"] = message.timestamp_ms >= 0 ? json(message.timestamp_ms) : json(nullptr);
        snapshot["observed_at"] = event.updated_at_iso;
        snapshot["key"] = message.key.empty() ? json(nullptr) : json(message.key);
        snapshot["payload"] = event.payload;
        archive.snapshots.push_back(snapshot);

        Candidate candidate;
        candidate.payload = event.payload;
        candidate.created_at = event.created_at;
        candidate.updated_at = event.updated_at;
        candidate.sequence = sequence;
        candidate.timestamp_ms = message.timestamp_ms;
        candidate.offset = message.offset;
        candidate.partition = message.partition;
        candidate.snapshot_index = archive.snapshots.size() - 1;

        std::string fingerprint = json_dump_sorted(event.payload);
        auto& seq_map = by_sequence[{account_id, order_id}];
        auto seen = seq_map.find(sequence);
        if (seen == seq_map.end()) seq_map[sequence] = {fingerprint, {message.offset}};
        else {
            seen->second.second.push_back(message.offset);
            if (seen->second.first != fingerprint) {
                archive.conflicts.push_back(json{{"account_id", account_id}, {"order_id", order_id}, {"sequence", sequence},
                                                 {"offsets", seen->second.second}});
            }
        }
        auto key = std::make_pair(account_id, order_id);
        auto it = latest.find(key);
        if (it == latest.end() || newer(candidate, it->second)) latest[key] = std::move(candidate);
    }

    for (auto& [partition, range] : bounds) archive.bounds[std::to_string(partition)] = json::array({range.first, range.second});

    std::vector<const Candidate*> ordered;
    for (auto& [key, candidate] : latest) ordered.push_back(&candidate);
    std::sort(ordered.begin(), ordered.end(), [](const Candidate* a, const Candidate* b) {
        if (a->created_at != b->created_at) return a->created_at < b->created_at;
        if (a->payload.value("account_id", "") != b->payload.value("account_id", "")) return a->payload.value("account_id", "") < b->payload.value("account_id", "");
        return a->payload.value("order_id", "") < b->payload.value("order_id", "");
    });
    for (const Candidate* candidate : ordered) {
        json row = candidate->payload;
        const AccountMeta* meta = accounts.find(row.value("account_id", ""));
        if (!row.contains("account_num_id") || row.at("account_num_id").is_null()) {
            row["account_num_id"] = meta ? json(meta->num_id) : json(nullptr);
        }
        row["broker"] = meta ? json(meta->broker) : json(nullptr);
        row["trading_medium"] = meta ? json(meta->trading_medium) : json(nullptr);
        row["created_at_" + suffix] = zone.iso(candidate->created_at);
        row["updated_at_" + suffix] = zone.iso(candidate->updated_at);
        row["source_offset"] = candidate->offset;
        row["source_partition"] = candidate->partition;
        archive.orders.push_back(std::move(row));
    }
    return archive;
}

ArchiveFiles write_day_archive(const DayArchive& archive, const std::filesystem::path& reports_dir) {
    ArchiveFiles files;
    files.directory = reports_dir / ("orders-" + archive.date);
    std::error_code ec;
    std::filesystem::create_directories(files.directory, ec);
    if (ec) throw std::runtime_error("cannot create " + files.directory.string() + ": " + ec.message());
    files.orders_file = files.directory / ("orders-" + archive.date + ".json");
    files.snapshots_file = files.directory / "source-snapshots.json";

    json orders_doc = archive.metadata();
    orders_doc["orders"] = archive.orders;
    write_atomic(files.orders_file, orders_doc.dump(2) + "\n");

    json snapshots_doc;
    snapshots_doc["date"] = archive.date;
    snapshots_doc["timezone"] = archive.timezone;
    snapshots_doc["topic"] = archive.topic;
    snapshots_doc["retrieved_at"] = iso_utc(archive.retrieved_at);
    snapshots_doc["bounds"] = archive.bounds;
    snapshots_doc["messages_read"] = archive.messages_read;
    snapshots_doc["matching_updates"] = archive.matching_updates;
    snapshots_doc["snapshots"] = archive.snapshots;
    write_atomic(files.snapshots_file, snapshots_doc.dump(2) + "\n");
    return files;
}

std::vector<RawMessage> read_retained_topic(const OrderArchiveConfig& config, const KafkaConfig& kafka, int timeout_seconds) {
    std::string bootstrap = config.bootstrap_servers.value_or(kafka.bootstrap_servers);
    std::string topic = config.topic;
    kafka::Config conf;
    conf.set("bootstrap.servers", bootstrap)
        .set("group.id", "ktrader-order-archive-" + random_token(8))
        .set("enable.auto.commit", "false")
        .set("auto.offset.reset", "earliest")
        .set("enable.partition.eof", "true")
        .set("allow.auto.create.topics", "false")
        .set("session.timeout.ms", "10000")
        .apply(config.client_config)
        .route_logs(kLog);
    kafka::Consumer::Options options;
    options.seek_to_beginning_on_assign = true;
    std::mutex assign_mutex;
    std::set<int> assigned;
    bool assignment_seen = false;
    options.on_assign = [&](std::vector<kafka::TopicPartition>& partitions) {
        std::lock_guard lock(assign_mutex);
        assignment_seen = true;
        for (auto& tp : partitions) assigned.insert(tp.partition);
    };
    std::string last_error;
    options.on_error = [&](int, const std::string& reason) { last_error = reason; };
    kafka::Consumer consumer(conf, options);
    consumer.subscribe({topic});

    std::vector<RawMessage> messages;
    std::set<int> eof;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        auto message = consumer.poll(250);
        {
            std::lock_guard lock(assign_mutex);
            if (assignment_seen && !assigned.empty() && std::includes(eof.begin(), eof.end(), assigned.begin(), assigned.end())) break;
            if (assignment_seen && assigned.empty()) {
                consumer.close();
                throw std::runtime_error("no partitions were assigned for topic " + topic + (last_error.empty() ? "" : ": " + last_error));
            }
        }
        if (!message) continue;
        if (message->eof) {
            eof.insert(message->partition);
            continue;
        }
        if (message->error) {
            consumer.close();
            throw std::runtime_error("consumer error on " + topic + ": " + message->error_text);
        }
        RawMessage raw;
        raw.topic = message->topic;
        raw.partition = message->partition;
        raw.offset = message->offset;
        raw.timestamp_ms = message->timestamp_ms;
        raw.key = message->key;
        raw.value = message->value;
        raw.tombstone = message->value.empty();
        messages.push_back(std::move(raw));
    }
    consumer.close();
    {
        std::lock_guard lock(assign_mutex);
        if (!assignment_seen || !std::includes(eof.begin(), eof.end(), assigned.begin(), assigned.end())) {
            throw std::runtime_error("timed out after " + std::to_string(timeout_seconds) + "s reading " + topic + " from " + bootstrap +
                                     (last_error.empty() ? "" : ": " + last_error));
        }
    }
    std::sort(messages.begin(), messages.end(), [](const RawMessage& a, const RawMessage& b) {
        if (a.partition != b.partition) return a.partition < b.partition;
        return a.offset < b.offset;
    });
    return messages;
}

json persist_day_orders(const AppConfig& config, const AccountDirectory& accounts, const std::optional<std::string>& date_in) {
    const OrderArchiveConfig& cfg = config.persistence.order_archive;
    auto zone = TimeZone::find(cfg.timezone);
    if (!zone) throw std::runtime_error("unsupported persistence.order_archive.timezone " + cfg.timezone);
    double now = wall_now();
    std::string date = date_in.value_or(zone->date(now));
    int y, m, d;
    if (!parse_civil_date(date, y, m, d)) throw std::runtime_error("date must be YYYY-MM-DD, got " + date);
    NG_LOG_INFO(kLog, "persisting orders of {} ({}) from topic {}", date, zone->name(), cfg.topic);
    std::vector<RawMessage> messages = read_retained_topic(cfg, config.kafka, cfg.read_timeout_seconds);
    DayArchive archive = build_day_archive(date, messages, accounts, *zone, cfg.topic, now);
    ArchiveFiles files = write_day_archive(archive, cfg.dir);
    json result = archive.metadata();
    result["files"] = json{{"directory", files.directory.string()}, {"orders", files.orders_file.string()}, {"snapshots", files.snapshots_file.string()}};
    NG_LOG_INFO(kLog, "wrote {} orders ({} matching updates of {} messages) to {}", archive.orders.size(), archive.matching_updates,
                archive.messages_read, files.directory.string());
    return result;
}

OrderArchiveScheduler::OrderArchiveScheduler(const AppConfig& config, const AccountDirectory& accounts)
    : config_(config), accounts_(accounts), zone_(TimeZone::find(config.persistence.order_archive.timezone).value_or(*TimeZone::find("UTC"))) {}

OrderArchiveScheduler::~OrderArchiveScheduler() { stop(); }

void OrderArchiveScheduler::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this] { loop(); });
}

void OrderArchiveScheduler::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

json OrderArchiveScheduler::status() const {
    std::lock_guard lock(mutex_);
    json out;
    out["enabled"] = true;
    out["timezone"] = zone_.name();
    out["daily_at"] = config_.persistence.order_archive.daily_at ? json(*config_.persistence.order_archive.daily_at) : json(nullptr);
    out["directory"] = config_.persistence.order_archive.dir;
    out["next_run"] = next_run_ > 0 ? json(iso_utc(next_run_)) : json(nullptr);
    out["last_result"] = last_result_;
    out["last_error"] = last_error_.empty() ? json(nullptr) : json(last_error_);
    return out;
}

void OrderArchiveScheduler::loop() {
    const OrderArchiveConfig& cfg = config_.persistence.order_archive;
    if (!cfg.daily_at) return;
    int hour = 17, minute = 0;
    std::sscanf(cfg.daily_at->c_str(), "%d:%d", &hour, &minute);
    for (;;) {
        double now = wall_now();
        std::string today = zone_.date(now);
        std::optional<double> next = zone_.epoch(today, hour, minute);
        if (!next) return;
        if (*next <= now) {
            // Tomorrow, same local time.
            int y, m, d;
            parse_civil_date(today, y, m, d);
            long long days = days_from_civil(y, m, d) + 1;
            civil_from_days(days, y, m, d);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
            next = zone_.epoch(buf, hour, minute);
            if (!next) return;
        }
        {
            std::unique_lock lock(mutex_);
            next_run_ = *next;
            while (!stop_ && wall_now() < *next) {
                double remaining = *next - wall_now();
                cv_.wait_for(lock, std::chrono::milliseconds(static_cast<long long>(std::min(remaining, 30.0) * 1000)));
            }
            if (stop_) return;
        }
        std::string date = zone_.date(*next);
        try {
            json result = persist_day_orders(config_, accounts_, date);
            std::lock_guard lock(mutex_);
            last_result_ = result;
            last_error_.clear();
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "daily order archive for {} failed: {}", date, e.what());
            std::lock_guard lock(mutex_);
            last_error_ = e.what();
        }
        // Avoid re-running within the same minute.
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(61), [this] { return stop_; });
        if (stop_) return;
    }
}

}  // namespace ng::ops
