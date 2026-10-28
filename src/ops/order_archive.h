// End-of-day order persistence: reads the retained order-status topic from the beginning, keeps
// every valid snapshot whose order was created on the archived (local) date, and writes
//
//   <dir>/orders-<date>/orders-<date>.json      latest state of every order of that day
//   <dir>/orders-<date>/source-snapshots.json   every matching raw snapshot with Kafka coordinates
//
// in the layout of the daily files produced on the trading box (`saved_order_export` /
// `saved_order_snapshots` sources of the order-history export). Runs from the daily scheduler
// inside the server or on demand with `nextgen-gui --persist-orders [--date YYYY-MM-DD]`.
#pragma once

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "core/time_zone.h"
#include "model/account.h"

namespace ng::ops {

struct RawMessage {
    std::string topic;
    int partition = 0;
    long long offset = 0;
    long long timestamp_ms = -1;  // broker/producer timestamp, -1 when unavailable
    std::string key;
    std::string value;  // empty with tombstone=true for deletions
    bool tombstone = false;
};

struct DayArchive {
    std::string date;
    std::string timezone;
    double retrieved_at = 0.0;
    std::string topic;
    json bounds = json::object();  // {"<partition>": [first_offset, last_offset]}
    int messages_read = 0;
    int matching_updates = 0;
    json configured_accounts = json::object();
    json orders = json::array();     // latest state per (account_id, order_id)
    json snapshots = json::array();  // every matching raw snapshot, in topic order
    json invalid = json::array();    // [{partition, offset, error}]
    json tombstones = json::array(); // [{partition, offset}]
    json conflicts = json::array();  // same (account, order, sequence) with different payloads

    json metadata() const;  // the top-level fields of orders-<date>.json without the rows
};

// Pure: filters and ranks the messages for `date` in `zone`.
DayArchive build_day_archive(const std::string& date, const std::vector<RawMessage>& messages, const AccountDirectory& accounts,
                             const TimeZone& zone, const std::string& topic, double retrieved_at);

struct ArchiveFiles {
    std::filesystem::path directory;
    std::filesystem::path orders_file;
    std::filesystem::path snapshots_file;
};

// Writes both files atomically (tmp + rename) under <reports_dir>/orders-<date>/.
ArchiveFiles write_day_archive(const DayArchive& archive, const std::filesystem::path& reports_dir);

// Reads every retained message of the topic (all partitions, from the beginning until each
// partition reports end-of-file). Throws std::runtime_error on timeouts or broker errors.
std::vector<RawMessage> read_retained_topic(const OrderArchiveConfig& config, const KafkaConfig& kafka, int timeout_seconds);

// Full pass: read, build, write. `date` defaults to today's date in the configured zone.
// Returns the archive metadata plus "files".
json persist_day_orders(const AppConfig& config, const AccountDirectory& accounts, const std::optional<std::string>& date);

// Runs persist_day_orders every day at `daily_at` (local time of the configured zone).
class OrderArchiveScheduler {
public:
    OrderArchiveScheduler(const AppConfig& config, const AccountDirectory& accounts);
    ~OrderArchiveScheduler();
    void start();
    void stop();
    json status() const;

private:
    void loop();
    const AppConfig& config_;
    const AccountDirectory& accounts_;
    TimeZone zone_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    json last_result_ = nullptr;
    std::string last_error_;
    double next_run_ = 0.0;
};

}  // namespace ng::ops
