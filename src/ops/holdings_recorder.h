// Account holdings recorder: every `interval_seconds` (aligned to the wall clock) the current
// holdings of every configured account — positions, cash by currency, trading state and the
// age of the underlying broker snapshot — are appended to
//
//   <dir>/holdings-<local date>.jsonl   one JSON document per tick
//   <dir>/holdings.sqlite3              the same data in queryable tables
//
// Both outputs are optional; the directory is configurable (persistence.account_snapshots.dir /
// ACCOUNT_SNAPSHOTS_DIR).
#pragma once

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/json.h"
#include "core/sqlite.h"
#include "core/time_zone.h"
#include "model/account.h"

namespace ng::ops {

class HoldingsRecorder {
public:
    using SnapshotsProvider = std::function<std::map<std::string, AccountSnapshot>()>;

    HoldingsRecorder(const HoldingsRecorderConfig& config, const AccountDirectory& accounts, SnapshotsProvider snapshots);
    ~HoldingsRecorder();

    // Builds the document for `wall` and persists it; returns the document. Throws on I/O errors.
    json record_once(double wall);
    void start();
    void stop();
    json status() const;

private:
    json build_document(double wall) const;
    void append_jsonl(const json& document, const std::string& date);
    void insert_sqlite(const json& document);
    void loop();

    HoldingsRecorderConfig config_;
    const AccountDirectory& accounts_;
    SnapshotsProvider snapshots_;
    TimeZone zone_;
    std::unique_ptr<Sqlite> db_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    long long recorded_ = 0;
    double last_taken_ = 0.0;
    std::string last_error_;
};

}  // namespace ng::ops
