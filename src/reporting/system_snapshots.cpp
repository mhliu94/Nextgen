#include "reporting/system_snapshots.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/logging.h"
#include "core/sqlite.h"
#include "core/time_util.h"

namespace ng::reporting {
namespace {

constexpr const char* kLog = "trading-ui.system-snapshots";

bool finite_values(const AccountSnapshot& snapshot) {
    if (!std::isfinite(snapshot.cash)) return false;
    for (auto& [currency, amount] : snapshot.cash_by_currency) {
        if (!std::isfinite(amount)) return false;
    }
    for (auto& [currency, amount] : snapshot.available_cash_by_currency) {
        if (!std::isfinite(amount)) return false;
    }
    for (const auto& position : snapshot.positions) {
        if (!std::isfinite(position.qty)) return false;
        if (position.avg_price && !std::isfinite(*position.avg_price)) return false;
        if (position.available_qty && !std::isfinite(*position.available_qty)) return false;
    }
    return true;
}

}  // namespace

SystemSnapshotRecorder::SystemSnapshotRecorder(const std::string& db_path, const AccountDirectory& accounts,
                                               SnapshotsProvider snapshots, double interval_seconds,
                                               std::optional<std::string> source, Clock monotonic, Clock wall_clock)
    : db_path_(db_path),
      accounts_(accounts),
      snapshots_(std::move(snapshots)),
      interval_seconds_(interval_seconds),
      source_(std::move(source)),
      monotonic_(monotonic ? std::move(monotonic) : Clock(&mono_now)),
      wall_clock_(wall_clock ? std::move(wall_clock) : Clock(&wall_now)) {
    if (!std::isfinite(interval_seconds) || interval_seconds <= 0) {
        throw std::invalid_argument("Snapshot interval must be a positive finite number.");
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(db_path_).parent_path(), ec);
    Sqlite db(db_path_);
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("CREATE TABLE IF NOT EXISTS system_snapshots ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "recorded_at TEXT NOT NULL, payload TEXT NOT NULL)");
    db.exec("CREATE INDEX IF NOT EXISTS system_snapshots_recorded_at ON system_snapshots(recorded_at)");
}

SystemSnapshotRecorder::~SystemSnapshotRecorder() { stop(); }

void SystemSnapshotRecorder::start() {
    if (thread_.joinable()) return;
    {
        std::lock_guard lock(wait_mutex_);
        stop_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void SystemSnapshotRecorder::stop() {
    {
        std::lock_guard lock(wait_mutex_);
        stop_ = true;
    }
    wait_cv_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void SystemSnapshotRecorder::run() {
    double next_run = monotonic_();
    while (true) {
        {
            std::lock_guard lock(wait_mutex_);
            if (stop_) return;
        }
        try {
            record_snapshot();
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "Failed recording system snapshot path={}: {}", db_path_, e.what());
            std::lock_guard lock(status_mutex_);
            last_error_ = e.what();
        }
        next_run += interval_seconds_;
        double current = monotonic_();
        // Do not create bursts of duplicate snapshots after slow storage.
        if (next_run <= current) {
            double missed = std::floor((current - next_run) / interval_seconds_) + 1;
            next_run += missed * interval_seconds_;
        }
        double wait = std::max(0.0, next_run - monotonic_());
        if (waiter_) {
            if (waiter_(wait)) return;
            continue;
        }
        std::unique_lock lock(wait_mutex_);
        if (wait_cv_.wait_for(lock, std::chrono::duration<double>(wait), [this] { return stop_; })) return;
    }
}

json SystemSnapshotRecorder::account_entry(const AccountMeta& meta, const AccountSnapshot* snapshot, double recorded_at) {
    json account;
    account["account_id"] = meta.id;
    account["account_num_id"] = meta.num_id;
    account["broker"] = meta.broker;
    account["trading_medium"] = meta.trading_medium;
    account["data_status"] = "missing";
    for (const char* key : {"cash", "cash_by_currency", "available_cash_by_currency", "positions", "ts", "trading_enabled",
                            "snapshot_age_seconds"}) {
        account[key] = nullptr;
    }
    if (!snapshot) return account;
    // One malformed account must not prevent recording all other accounts.
    if (snapshot->account_id != meta.id || (snapshot->account_num_id && *snapshot->account_num_id != meta.num_id) ||
        !finite_values(*snapshot)) {
        account["data_status"] = "invalid";
        return account;
    }
    account.update(snapshot->to_json());
    account["account_num_id"] = meta.num_id;
    account["data_status"] = "available";
    if (snapshot->ts && !snapshot->ts->empty()) {
        auto source_time = parse_iso8601(*snapshot->ts);
        if (source_time && source_time->has_offset) {
            // timedelta.total_seconds(): the difference in whole microseconds.
            long long micros = std::llround(recorded_at * 1e6) - std::llround(source_time->epoch_seconds * 1e6);
            account["snapshot_age_seconds"] = static_cast<double>(micros) / 1e6;
        }
    }
    return account;
}

json SystemSnapshotRecorder::record_snapshot() {
    // Microsecond resolution, like the Python recorder's datetimes.
    double recorded_at = static_cast<double>(std::llround(wall_clock_() * 1e6)) / 1e6;
    std::map<std::string, AccountSnapshot> snapshots;
    bool provider_failed = false;
    try {
        snapshots = snapshots_();
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "Failed reading accounts for system snapshot: {}", e.what());
        provider_failed = true;
    }
    json document;
    document["schema_version"] = 1;
    document["recorded_at"] = iso_utc(recorded_at);
    document["source"] = (source_ && !provider_failed) ? json(*source_) : json(nullptr);
    document["account_count"] = static_cast<long long>(accounts_.size());
    document["accounts"] = json::array();
    std::vector<const AccountMeta*> ordered;
    for (const AccountMeta& meta : accounts_.ordered()) ordered.push_back(&meta);
    std::stable_sort(ordered.begin(), ordered.end(), [](const AccountMeta* a, const AccountMeta* b) {
        return a->num_id != b->num_id ? a->num_id < b->num_id : a->id < b->id;
    });
    for (const AccountMeta* meta : ordered) {
        auto it = snapshots.find(meta->id);
        document["accounts"].push_back(account_entry(*meta, it == snapshots.end() ? nullptr : &it->second, recorded_at));
    }
    if (provider_failed) document["error"] = "snapshot_provider_error";
    std::string payload = json_dump_compact(document);
    {
        Sqlite db(db_path_);
        db.begin_immediate();
        try {
            Statement insert(db, "INSERT INTO system_snapshots(recorded_at, payload) VALUES (?, ?)");
            insert.bind(1, document.at("recorded_at").get<std::string>()).bind(2, payload);
            insert.exec();
            db.commit();
        } catch (...) {
            db.rollback();
            throw;
        }
    }
    std::lock_guard lock(status_mutex_);
    ++recorded_;
    last_recorded_at_ = document.at("recorded_at").get<std::string>();
    last_error_.clear();
    return document;
}

json SystemSnapshotRecorder::status() const {
    std::lock_guard lock(status_mutex_);
    json out;
    out["enabled"] = true;
    out["path"] = db_path_;
    out["interval_seconds"] = interval_seconds_;
    out["snapshots"] = recorded_;
    out["last_recorded_at"] = last_recorded_at_.empty() ? json(nullptr) : json(last_recorded_at_);
    out["last_error"] = last_error_.empty() ? json(nullptr) : json(last_error_);
    return out;
}

}  // namespace ng::reporting
