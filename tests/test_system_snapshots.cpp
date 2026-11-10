// Minute-by-minute account holdings and cash snapshots (tests/test_system_snapshots.py).
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#include "core/sqlite.h"
#include "core/time_util.h"
#include "reporting/system_snapshots.h"
#include "test_helpers.h"

using ng::AccountSnapshot;
using ng::json;
using ng::Position;
using ng::reporting::SystemSnapshotRecorder;

namespace {

// 2026-10-28T15:00:00Z
const double NOW = ng::parse_iso8601("2026-10-28T15:00:00+00:00")->epoch_seconds;

struct SnapshotFixture {
    ngtest::TempDir dir{"system-snapshots"};
    // Deliberately configured out of order: documents sort by numeric id.
    ng::AccountDirectory accounts = ngtest::directory({ngtest::meta("B", 2, "Futu"), ngtest::meta("A", 1, "Tiger")});
    std::string path() const { return dir.file("activity.sqlite3"); }

    std::unique_ptr<SystemSnapshotRecorder> recorder(SystemSnapshotRecorder::SnapshotsProvider provider = nullptr,
                                                     double interval = 60, SystemSnapshotRecorder::Clock monotonic = nullptr) {
        if (!provider) provider = [] { return std::map<std::string, AccountSnapshot>{}; };
        return std::make_unique<SystemSnapshotRecorder>(path(), accounts, std::move(provider), interval, std::string("kafka"),
                                                        std::move(monotonic), [] { return NOW; });
    }
    long long count() const {
        ng::Sqlite db(path(), ng::Sqlite::Mode::ReadOnly);
        ng::Statement rows(db, "SELECT count(*) FROM system_snapshots");
        REQUIRE(rows.step());
        return rows.column_int(0);
    }
};

AccountSnapshot simple(const std::string& id, double cash) {
    AccountSnapshot s;
    s.account_id = id;
    s.cash = cash;
    return s;
}

}  // namespace

TEST_CASE_FIXTURE(SnapshotFixture, "records every account and preserves cash, holdings and source age") {
    AccountSnapshot snapshot;
    snapshot.account_id = "A";
    snapshot.cash = 500;
    snapshot.cash_by_currency = {{"USD", 500}, {"HKD", 1000}};
    snapshot.available_cash_by_currency = {{"USD", 450}};
    snapshot.positions.push_back(Position{"AAPL", 1.5, 200.0, 1.0});
    snapshot.ts = "2026-10-28T14:55:00Z";
    snapshot.trading_enabled = true;
    auto service = recorder([&] { return std::map<std::string, AccountSnapshot>{{"A", snapshot}, {"UNKNOWN", snapshot}}; });
    json document = service->record_snapshot();

    CHECK(document["schema_version"] == 1);
    CHECK(document["source"] == "kafka");
    CHECK(document["account_count"] == 2);
    REQUIRE(document["accounts"].size() == 2);
    const json& first = document["accounts"][0];
    const json& missing = document["accounts"][1];
    CHECK(first["account_id"] == "A");
    CHECK(first["account_num_id"] == 1);
    CHECK(first["broker"] == "Tiger");
    CHECK(first["trading_medium"] == "API");
    CHECK(first["cash"] == 500.0);
    CHECK(first["cash_by_currency"] == json{{"USD", 500.0}, {"HKD", 1000.0}});
    CHECK(first["available_cash_by_currency"] == json{{"USD", 450.0}});
    CHECK(first["positions"] == json::array({{{"symbol", "AAPL"}, {"qty", 1.5}, {"avg_price", 200.0}, {"available_qty", 1.0}}}));
    CHECK(first["ts"] == "2026-10-28T14:55:00Z");
    CHECK(first["trading_enabled"] == true);
    CHECK(first["snapshot_age_seconds"] == 300.0);
    CHECK(first["data_status"] == "available");
    CHECK(missing["account_id"] == "B");
    CHECK(missing["data_status"] == "missing");
    for (const char* key : {"cash", "cash_by_currency", "available_cash_by_currency", "positions", "ts", "trading_enabled",
                            "snapshot_age_seconds"}) {
        CHECK(missing[key].is_null());
    }

    ng::Sqlite db(path(), ng::Sqlite::Mode::ReadOnly);
    ng::Statement row(db, "SELECT recorded_at, payload FROM system_snapshots");
    REQUIRE(row.step());
    CHECK(row.column_text(0) == "2026-10-28T15:00:00Z");
    CHECK(json::parse(row.column_text(1)) == document);
    ng::Statement mode(db, "PRAGMA journal_mode");
    REQUIRE(mode.step());
    CHECK(mode.column_text(0) == "wal");
    json status = service->status();
    CHECK(status["snapshots"] == 1);
    CHECK(status["last_recorded_at"] == "2026-10-28T15:00:00Z");
    CHECK(status["last_error"].is_null());
}

TEST_CASE_FIXTURE(SnapshotFixture, "snapshots survive recorder restart and preserve other tables") {
    recorder()->record_snapshot();
    {
        ng::Sqlite db(path());
        db.exec("CREATE TABLE activity_test (id INTEGER)");
        db.exec("INSERT INTO activity_test VALUES (42)");
    }
    recorder()->record_snapshot();
    CHECK(count() == 2);
    ng::Sqlite db(path(), ng::Sqlite::Mode::ReadOnly);
    ng::Statement row(db, "SELECT id FROM activity_test");
    REQUIRE(row.step());
    CHECK(row.column_int(0) == 42);
}

TEST_CASE_FIXTURE(SnapshotFixture, "provider failure is documented without fabricating empty balances") {
    auto service = recorder([]() -> std::map<std::string, AccountSnapshot> { throw std::runtime_error("account feed unavailable"); });
    json document = service->record_snapshot();
    CHECK(document["error"] == "snapshot_provider_error");
    CHECK(document["source"].is_null());
    CHECK(document["account_count"] == 2);
    for (const auto& account : document["accounts"]) {
        CHECK(account["data_status"] == "missing");
        CHECK(account["cash"].is_null());
    }
}

TEST_CASE_FIXTURE(SnapshotFixture, "invalid account does not prevent other accounts from being recorded") {
    AccountSnapshot not_a_number = simple("A", std::numeric_limits<double>::quiet_NaN());
    AccountSnapshot wrong_id = simple("WRONG", 1);
    AccountSnapshot wrong_number = simple("A", 1);
    wrong_number.account_num_id = 99;
    AccountSnapshot bad_position = simple("A", 1);
    bad_position.positions.push_back(Position{"AAPL", std::numeric_limits<double>::infinity(), std::nullopt, std::nullopt});
    for (const AccountSnapshot& invalid : {not_a_number, wrong_id, wrong_number, bad_position}) {
        auto service = recorder([&] { return std::map<std::string, AccountSnapshot>{{"A", invalid}, {"B", simple("B", 25)}}; });
        json document = service->record_snapshot();
        CHECK(document["accounts"][0]["data_status"] == "invalid");
        CHECK(document["accounts"][0]["cash"].is_null());
        CHECK(document["accounts"][1]["cash"] == 25.0);
        CHECK(document["accounts"][1]["data_status"] == "available");
        // No source time: the age is unknown, never zero.
        CHECK(document["accounts"][1]["snapshot_age_seconds"].is_null());
    }
}

TEST_CASE_FIXTURE(SnapshotFixture, "start records immediately and stop interrupts the minute wait") {
    std::atomic<int> captured{0};
    auto service = recorder([&] {
        ++captured;
        return std::map<std::string, AccountSnapshot>{};
    });
    CHECK(service->interval_seconds() == 60);
    service->start();
    service->start();  // idempotent while the worker is alive
    for (int i = 0; i < 400 && captured.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(captured.load() == 1);
    for (int i = 0; i < 400 && service->status()["snapshots"] == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto before = std::chrono::steady_clock::now();
    service->stop();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(5));
    CHECK(count() == 1);
}

TEST_CASE_FIXTURE(SnapshotFixture, "worker retries after a storage failure") {
    auto service = recorder(nullptr, 0.01);
    {
        // The next insert fails until the table is back.
        ng::Sqlite db(path());
        db.exec("ALTER TABLE system_snapshots RENAME TO system_snapshots_gone");
    }
    service->start();
    for (int i = 0; i < 400 && service->status()["last_error"].is_null(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(service->status()["last_error"].is_string());
    CHECK(service->status()["snapshots"] == 0);
    {
        ng::Sqlite db(path());
        db.exec("ALTER TABLE system_snapshots_gone RENAME TO system_snapshots");
    }
    for (int i = 0; i < 400 && service->status()["snapshots"] == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    service->stop();
    CHECK(service->status()["snapshots"].get<long long>() >= 1);
    CHECK(count() >= 1);
}

TEST_CASE_FIXTURE(SnapshotFixture, "cadence does not drift or burst after slow recording") {
    struct Case { double record_duration, expected_wait; };
    for (Case c : {Case{7, 53}, Case{135, 45}}) {
        CAPTURE(c.record_duration);
        double ticks = 0;
        std::vector<double> waits;
        auto service = recorder(
            [&] {
                ticks += c.record_duration;  // recording takes this long on the simulated clock
                return std::map<std::string, AccountSnapshot>{};
            },
            60, [&] { return ticks; });
        service->set_waiter_for_test([&](double seconds) {
            waits.push_back(seconds);
            ticks += seconds;
            return waits.size() == 2;
        });
        service->run_for_test();
        CHECK(waits == std::vector<double>{c.expected_wait, c.expected_wait});
    }
}

TEST_CASE_FIXTURE(SnapshotFixture, "intervals must be positive and finite") {
    for (double interval : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_THROWS_WITH_AS(recorder(nullptr, interval), "Snapshot interval must be a positive finite number.", std::invalid_argument);
    }
}

TEST_CASE("account entries report sub-second source ages exactly") {
    ng::AccountMeta meta = ngtest::meta("A", 1, "Tiger");
    AccountSnapshot snapshot = simple("A", 1);
    snapshot.ts = "2026-10-28T10:59:59.250000-04:00";
    json entry = SystemSnapshotRecorder::account_entry(meta, &snapshot, NOW + 0.5);
    CHECK(entry["snapshot_age_seconds"] == 1.25);
    snapshot.ts = "2026-10-28T15:00:01Z";  // a source clock slightly ahead gives a negative age
    CHECK(SystemSnapshotRecorder::account_entry(meta, &snapshot, NOW)["snapshot_age_seconds"] == -1.0);
    snapshot.ts = "2026-10-28T15:00:00";  // no offset: the age is unknown
    CHECK(SystemSnapshotRecorder::account_entry(meta, &snapshot, NOW)["snapshot_age_seconds"].is_null());
    snapshot.ts = "not a time";
    json unknown = SystemSnapshotRecorder::account_entry(meta, &snapshot, NOW);
    CHECK(unknown["snapshot_age_seconds"].is_null());
    CHECK(unknown["data_status"] == "available");
    CHECK(unknown["ts"] == "not a time");
}
