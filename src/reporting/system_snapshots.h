// Durable, minute-by-minute account holdings and cash snapshots
// (trading_ui/services/system_snapshots.py), stored as JSON documents in the `system_snapshots`
// table of the activity reporting database.
//
// Every configured account is recorded, even before its first broker update and including accounts
// hidden in the UI. A recorded account contains the latest observed values, not a new broker query:
// `ts` and `snapshot_age_seconds` preserve their actual freshness. Missing or invalid values are
// explicitly unavailable rather than zero.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/json.h"
#include "model/account.h"

namespace ng::reporting {

class SystemSnapshotRecorder {
public:
    using SnapshotsProvider = std::function<std::map<std::string, AccountSnapshot>()>;
    using Clock = std::function<double()>;

    // `source` labels where the snapshots come from ("kafka"). Throws std::invalid_argument for a
    // non-positive interval.
    SystemSnapshotRecorder(const std::string& db_path, const AccountDirectory& accounts, SnapshotsProvider snapshots,
                           double interval_seconds = 60, std::optional<std::string> source = std::string("kafka"),
                           Clock monotonic = nullptr, Clock wall_clock = nullptr);
    ~SystemSnapshotRecorder();

    void start();
    void stop();
    // Records one document now and returns it.
    json record_snapshot();
    // {enabled, path, interval_seconds, snapshots, last_recorded_at, last_error} for /api/health.
    json status() const;

    // One account's entry (exposed for tests).
    static json account_entry(const AccountMeta& meta, const AccountSnapshot* snapshot, double recorded_at);

    double interval_seconds() const { return interval_seconds_; }
    // Test hook: replaces the interruptible wait between snapshots (returns true to stop the worker).
    void set_waiter_for_test(std::function<bool(double seconds)> waiter) { waiter_ = std::move(waiter); }
    // Runs the worker loop on the calling thread (tests drive it with a simulated clock and waiter).
    void run_for_test() { run(); }

private:
    void run();

    std::string db_path_;
    AccountDirectory accounts_;
    SnapshotsProvider snapshots_;
    double interval_seconds_;
    std::optional<std::string> source_;
    Clock monotonic_;
    Clock wall_clock_;
    std::function<bool(double)> waiter_;
    std::thread thread_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    bool stop_ = false;
    mutable std::mutex status_mutex_;
    long long recorded_ = 0;
    std::string last_recorded_at_;
    std::string last_error_;
};

}  // namespace ng::reporting
