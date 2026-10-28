#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/sqlite.h"
#include "core/time_util.h"
#include "core/time_zone.h"
#include "model/account.h"
#include "ops/holdings_recorder.h"
#include "ops/order_archive.h"
#include "test_helpers.h"

using ng::json;
using ng::TimeZone;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

ng::ops::RawMessage message(int offset, const json& payload, long long ts_ms = -1) {
    ng::ops::RawMessage m;
    m.topic = "order-status";
    m.partition = 0;
    m.offset = offset;
    m.timestamp_ms = ts_ms;
    m.value = payload.dump();
    return m;
}

json order(const std::string& account, const std::string& order_id, const std::string& created, const std::string& updated, long long sequence,
           const std::string& status, double filled) {
    return json{{"schema_version", 1}, {"account_id", account}, {"account_num_id", account == "A" ? 1 : 2}, {"order_id", order_id},
                {"sequence", sequence}, {"created_at", created}, {"updated_at", updated}, {"symbol", "XYZ"}, {"side", "SELL"},
                {"order_type", "LIMIT"}, {"status", status}, {"quantity", 100}, {"filled_quantity", filled},
                {"average_fill_price", filled > 0 ? json(10.0) : json(nullptr)}, {"limit_price", 10}, {"command_id", "cmd-" + order_id}};
}

}  // namespace

TEST_CASE("time zones: New York daylight saving rules, fixed zones and local dates") {
    auto ny = TimeZone::find("America/New_York");
    REQUIRE(ny);
    // 2026-03-08 06:59:59 UTC is still EST (-05:00); one second later DST starts (-04:00).
    auto before = ng::parse_iso8601("2026-03-08T06:59:59Z");
    auto after = ng::parse_iso8601("2026-03-08T07:00:00Z");
    REQUIRE(before);
    REQUIRE(after);
    CHECK(ny->offset_seconds(before->epoch_seconds) == -5 * 3600);
    CHECK(ny->offset_seconds(after->epoch_seconds) == -4 * 3600);
    CHECK(ny->iso(before->epoch_seconds) == "2026-03-08T01:59:59-05:00");
    CHECK(ny->iso(after->epoch_seconds) == "2026-03-08T03:00:00-04:00");
    // DST ends 2026-11-01 06:00 UTC.
    auto fall = ng::parse_iso8601("2026-11-01T05:59:59Z");
    auto winter = ng::parse_iso8601("2026-11-01T06:00:00Z");
    CHECK(ny->iso(fall->epoch_seconds) == "2026-11-01T01:59:59-04:00");
    CHECK(ny->iso(winter->epoch_seconds) == "2026-11-01T01:00:00-05:00");
    // The archive sample: 2026-10-21T13:52:51Z is 09:52:51 in New York.
    auto sample = ng::parse_iso8601("2026-10-21T13:52:51+00:00");
    CHECK(ny->iso(sample->epoch_seconds) == "2026-10-21T09:52:51-04:00");
    CHECK(ny->date(sample->epoch_seconds) == "2026-10-21");
    // Local midnight belongs to the local date, not the UTC date.
    auto late = ng::parse_iso8601("2026-10-22T03:30:00Z");
    CHECK(ny->date(late->epoch_seconds) == "2026-10-21");
    // Wall-clock to epoch and back.
    auto five_pm = ny->epoch("2026-10-21", 17, 0);
    REQUIRE(five_pm);
    CHECK(ng::iso_utc(*five_pm) == "2026-10-21T21:00:00Z");
    auto winter_five = ny->epoch("2026-12-01", 17, 0);
    CHECK(ng::iso_utc(*winter_five) == "2026-12-01T22:00:00Z");
    CHECK_FALSE(ny->epoch("2026-13-01", 17, 0));
    CHECK(ny->field_suffix() == "new_york");

    auto utc = TimeZone::find("UTC");
    REQUIRE(utc);
    CHECK(utc->iso(sample->epoch_seconds) == "2026-10-21T13:52:51+00:00");
    CHECK(utc->field_suffix() == "utc");
    auto shanghai = TimeZone::find("Asia/Shanghai");
    REQUIRE(shanghai);
    CHECK(shanghai->iso(sample->epoch_seconds) == "2026-10-21T21:52:51+08:00");
    CHECK(shanghai->field_suffix() == "shanghai");
    CHECK_FALSE(TimeZone::find("Mars/Olympus"));
    int y, m, d;
    CHECK(ng::parse_civil_date("2026-02-28", y, m, d));
    CHECK_FALSE(ng::parse_civil_date("2026-02-30", y, m, d));
    CHECK_FALSE(ng::parse_civil_date("20261021", y, m, d));
}

TEST_CASE("order archive keeps the day's orders at their latest state with provenance") {
    ngtest::TempDir dir{"archive"};
    auto accounts = ngtest::directory({ngtest::meta("A", 1, "GigaMoney", "EMULATOR"), ngtest::meta("B", 2, "Tiger", "API")});
    auto ny = *TimeZone::find("America/New_York");
    std::vector<ng::ops::RawMessage> messages;
    // Previous day (New York): excluded even though it is the same UTC date as the next entry.
    messages.push_back(message(10, order("A", "o-old", "2026-10-21T02:30:00+00:00", "2026-10-21T02:30:00+00:00", 0, "FILLED", 100), 1000));
    // The day's orders: two updates of the same order (sequence decides), one reordered.
    messages.push_back(message(11, order("A", "o-1", "2026-10-21T13:52:51+00:00", "2026-10-21T13:52:51+00:00", 0, "NEW", 0), 2000));
    messages.push_back(message(12, order("A", "o-1", "2026-10-21T13:52:51+00:00", "2026-10-21T13:53:10+00:00", 2, "FILLED", 100), 3000));
    messages.push_back(message(13, order("A", "o-1", "2026-10-21T13:52:51+00:00", "2026-10-21T13:53:00+00:00", 1, "PARTIALLY_FILLED", 40), 4000));
    messages.push_back(message(14, order("B", "o-2", "2026-10-21T20:00:00+00:00", "2026-10-21T20:00:00+00:00", 0, "REJECTED", 0), 5000));
    // Duplicate capture of offset 12's payload under a new offset: not a conflict.
    messages.push_back(message(15, order("A", "o-1", "2026-10-21T13:52:51+00:00", "2026-10-21T13:53:10+00:00", 2, "FILLED", 100), 6000));
    // Same sequence, different payload: recorded as a conflict.
    messages.push_back(message(16, order("B", "o-2", "2026-10-21T20:00:00+00:00", "2026-10-21T20:00:00+00:00", 0, "CANCELED", 0), 7000));
    // Unknown account: still archived (broker unknown).
    messages.push_back(message(17, order("Z", "o-3", "2026-10-21T21:00:00+00:00", "2026-10-21T21:00:00+00:00", 0, "FILLED", 100), 8000));
    // Next day in New York (after 20:00 local), a tombstone and garbage.
    messages.push_back(message(18, order("A", "o-4", "2026-10-22T04:30:00+00:00", "2026-10-22T04:30:00+00:00", 0, "FILLED", 100), 9000));
    ng::ops::RawMessage tombstone;
    tombstone.topic = "order-status";
    tombstone.offset = 19;
    tombstone.tombstone = true;
    messages.push_back(tombstone);
    ng::ops::RawMessage garbage = message(20, json::object());
    garbage.value = "{not json";
    messages.push_back(garbage);

    auto retrieved = ng::parse_iso8601("2026-10-21T22:00:00Z")->epoch_seconds;
    ng::ops::DayArchive archive = ng::ops::build_day_archive("2026-10-21", messages, accounts, ny, "order-status", retrieved);
    CHECK(archive.messages_read == 11);
    CHECK(archive.matching_updates == 7);
    CHECK(archive.bounds == json{{"0", json::array({10, 20})}});
    CHECK(archive.tombstones.size() == 1);
    CHECK(archive.invalid.size() == 1);
    CHECK(archive.invalid[0]["offset"] == 20);
    REQUIRE(archive.conflicts.size() == 1);
    CHECK(archive.conflicts[0]["order_id"] == "o-2");
    CHECK(archive.conflicts[0]["offsets"] == json::array({14, 16}));
    REQUIRE(archive.orders.size() == 3);
    const json& first = archive.orders[0];
    CHECK(first["order_id"] == "o-1");
    CHECK(first["status"] == "FILLED");
    CHECK(first["sequence"] == 2);
    CHECK(first["broker"] == "GigaMoney");
    CHECK(first["trading_medium"] == "EMULATOR");
    CHECK(first["created_at_new_york"] == "2026-10-21T09:52:51-04:00");
    CHECK(first["updated_at_new_york"] == "2026-10-21T09:53:10-04:00");
    CHECK(first["source_offset"] == 15);
    const json& second = archive.orders[1];
    CHECK(second["order_id"] == "o-2");
    CHECK(second["status"] == "CANCELED");  // later offset wins among equal sequence/updated_at
    const json& third = archive.orders[2];
    CHECK(third["account_id"] == "Z");
    CHECK(third["broker"].is_null());
    CHECK(third["account_num_id"] == 2);  // kept from the payload
    CHECK(archive.snapshots.size() == 7);
    CHECK(archive.snapshots[0]["offset"] == 11);
    CHECK(archive.snapshots[0]["timestamp_ms"] == 2000);
    CHECK(archive.configured_accounts["A"]["num_id"] == 1);

    ng::ops::ArchiveFiles files = ng::ops::write_day_archive(archive, dir.path);
    CHECK(files.directory == dir.path / "orders-2026-10-21");
    json orders_doc = json::parse(read_file(files.orders_file));
    CHECK(orders_doc["date"] == "2026-10-21");
    CHECK(orders_doc["timezone"] == "America/New_York");
    CHECK(orders_doc["source"] == "KTrader order-status retained Kafka snapshots");
    CHECK(orders_doc["retrieved_at"] == "2026-10-21T22:00:00Z");
    CHECK(orders_doc["matching_updates"] == 7);
    CHECK(orders_doc["orders"].size() == 3);
    json snapshots_doc = json::parse(read_file(files.snapshots_file));
    CHECK(snapshots_doc["snapshots"].size() == 7);
    CHECK(snapshots_doc["snapshots"][6]["payload"]["order_id"] == "o-3");
    CHECK_FALSE(std::filesystem::exists(files.orders_file.string() + ".tmp"));

    // Rewriting the same day replaces the files atomically.
    ng::ops::DayArchive again = ng::ops::build_day_archive("2026-10-21", {}, accounts, ny, "order-status", retrieved);
    ng::ops::write_day_archive(again, dir.path);
    CHECK(json::parse(read_file(files.orders_file))["orders"].empty());
}

TEST_CASE("holdings recorder appends a JSON line and SQLite rows per tick") {
    ngtest::TempDir dir{"holdings"};
    auto accounts = ngtest::directory({ngtest::meta("A", 1, "Tiger", "API"), ngtest::meta("B", 2, "GigaMoney", "EMULATOR")});
    ng::HoldingsRecorderConfig config;
    config.dir = dir.file("snapshots");
    config.timezone = "America/New_York";
    std::map<std::string, ng::AccountSnapshot> snapshots;
    ng::AccountSnapshot a;
    a.account_id = "A";
    a.cash = 1000.5;
    a.cash_by_currency = {{"USD", 1000.5}, {"HKD", 20}};
    a.available_cash_by_currency = {{"USD", 900}};
    a.positions.push_back(ng::Position{"XYZ", 150, 10.5, 150});
    a.positions.push_back(ng::Position{"ABC", 20, std::nullopt, std::nullopt});
    a.ts = "2026-10-21T13:52:00Z";
    a.trading_enabled = true;
    snapshots["A"] = a;
    ng::ops::HoldingsRecorder recorder(config, accounts, [&] { return snapshots; });
    double wall = ng::parse_iso8601("2026-10-21T13:53:00Z")->epoch_seconds;
    json doc = recorder.record_once(wall);
    CHECK(doc["taken_at"] == "2026-10-21T13:53:00Z");
    CHECK(doc["taken_at_local"] == "2026-10-21T09:53:00-04:00");
    CHECK(doc["date_local"] == "2026-10-21");
    CHECK(doc["account_count"] == 2);
    CHECK(doc["accounts_with_data"] == 1);
    REQUIRE(doc["accounts"].size() == 2);
    CHECK(doc["accounts"][0]["account_id"] == "A");
    CHECK(doc["accounts"][0]["has_snapshot"] == true);
    CHECK(doc["accounts"][0]["age_seconds"] == 60.0);
    CHECK(doc["accounts"][0]["positions"].size() == 2);
    CHECK(doc["accounts"][0]["positions"][1]["avg_price"].is_null());
    CHECK(doc["accounts"][1]["account_id"] == "B");
    CHECK(doc["accounts"][1]["has_snapshot"] == false);
    CHECK(doc["accounts"][1]["positions"].empty());
    recorder.record_once(wall + 60);

    std::string lines = read_file(std::filesystem::path(config.dir) / "holdings-2026-10-21.jsonl");
    CHECK(std::count(lines.begin(), lines.end(), '\n') == 2);
    CHECK(json::parse(lines.substr(0, lines.find('\n')))["accounts"][0]["cash"] == 1000.5);

    ng::Sqlite db((std::filesystem::path(config.dir) / "holdings.sqlite3").string());
    ng::Statement count(db, "SELECT COUNT(*) FROM snapshots");
    REQUIRE(count.step());
    CHECK(count.column_int(0) == 2);
    ng::Statement holdings(db, "SELECT COUNT(*), SUM(qty) FROM holdings WHERE account_id='A'");
    REQUIRE(holdings.step());
    CHECK(holdings.column_int(0) == 4);
    CHECK(holdings.column_double(1) == 340.0);
    ng::Statement cash(db, "SELECT currency, amount, available FROM cash WHERE snapshot_id=1 AND account_id='A' ORDER BY currency");
    REQUIRE(cash.step());
    CHECK(cash.column_text(0) == "HKD");
    CHECK(cash.is_null(2));
    REQUIRE(cash.step());
    CHECK(cash.column_text(0) == "USD");
    CHECK(cash.column_double(2) == 900.0);
    ng::Statement states(db, "SELECT has_snapshot, position_count FROM account_states WHERE snapshot_id=2 ORDER BY account_id");
    REQUIRE(states.step());
    CHECK(states.column_int(0) == 1);
    CHECK(states.column_int(1) == 2);
    REQUIRE(states.step());
    CHECK(states.column_int(0) == 0);
    json status = recorder.status();
    CHECK(status["recorded"] == 2);
    CHECK(status["last_taken_at"] == "2026-10-21T13:54:00Z");
}

TEST_CASE("persistence configuration: defaults, overrides and validation") {
    ngtest::TempDir dir{"persistence-config"};
    json raw = json::parse(R"({"order_archive": {"dir": "reports", "daily_at": "16:45", "timezone": "America/New_York", "topic": "orders-v1"},
                              "account_snapshots": {"dir": "snaps", "interval_seconds": 30, "formats": ["jsonl"]}})");
    ng::SmartSellingConfig smart;
    ng::PersistenceConfig cfg = ng::normalize_persistence_config(&raw, dir.path.string(), smart);
    CHECK(cfg.order_archive.enabled);
    CHECK(cfg.order_archive.dir == (dir.path / "reports").lexically_normal().string());
    CHECK(cfg.order_archive.daily_at == std::optional<std::string>("16:45"));
    CHECK(cfg.order_archive.topic == "orders-v1");
    CHECK(cfg.account_snapshots.dir == (dir.path / "snaps").lexically_normal().string());
    CHECK(cfg.account_snapshots.interval_seconds == 30);
    CHECK(cfg.account_snapshots.write_jsonl);
    CHECK_FALSE(cfg.account_snapshots.write_sqlite);

    json none = json::object();
    ng::PersistenceConfig defaults = ng::normalize_persistence_config(&none, dir.path.string(), smart);
    CHECK(defaults.order_archive.topic == "order-status");
    CHECK(defaults.order_archive.daily_at == std::optional<std::string>("17:00"));
    CHECK(defaults.account_snapshots.interval_seconds == 60);
    CHECK(defaults.account_snapshots.write_sqlite);
    CHECK(defaults.account_snapshots.dir == (dir.path / "snapshots").lexically_normal().string());

    json bad_zone = json::parse(R"({"order_archive": {"timezone": "Mars/Olympus"}})");
    CHECK_THROWS_AS(ng::normalize_persistence_config(&bad_zone, dir.path.string(), smart), ng::ConfigError);
    json bad_time = json::parse(R"({"order_archive": {"daily_at": "25:00"}})");
    CHECK_THROWS_AS(ng::normalize_persistence_config(&bad_time, dir.path.string(), smart), ng::ConfigError);
    json disabled = json::parse(R"({"order_archive": {"daily_at": null, "enabled": false}})");
    ng::PersistenceConfig off = ng::normalize_persistence_config(&disabled, dir.path.string(), smart);
    CHECK_FALSE(off.order_archive.enabled);
    CHECK_FALSE(off.order_archive.daily_at);

    setenv("ORDER_ARCHIVE_DIR", "/var/tmp/ktrader-reports", 1);
    setenv("ACCOUNT_SNAPSHOTS_DIR", "/var/tmp/ktrader-snapshots", 1);
    ng::PersistenceConfig env = ng::normalize_persistence_config(&raw, dir.path.string(), smart);
    CHECK(env.order_archive.dir == "/var/tmp/ktrader-reports");
    CHECK(env.account_snapshots.dir == "/var/tmp/ktrader-snapshots");
    unsetenv("ORDER_ARCHIVE_DIR");
    unsetenv("ACCOUNT_SNAPSHOTS_DIR");
}
