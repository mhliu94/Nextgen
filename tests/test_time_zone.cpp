// Time zones read from TZif files (any IANA name, as Python's zoneinfo), checked against zoneinfo.
#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/time_util.h"
#include "core/time_zone.h"
#include "test_helpers.h"

using ng::TimeZone;

namespace {

#include "fixtures/zone_vectors.inc"

// The embedded TZif files in a private zoneinfo directory, with TZDIR pointing at it.
struct ZoneDir {
    ngtest::TempDir dir{"zoneinfo"};
    std::optional<std::string> previous;
    ZoneDir() {
        for (const auto& file : kTzifFiles) {
            std::filesystem::path path = dir.path / file.name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(file.data), static_cast<std::streamsize>(file.size));
        }
        if (const char* old = std::getenv("TZDIR")) previous = old;
        setenv("TZDIR", dir.path.c_str(), 1);
    }
    ~ZoneDir() {
        if (previous) setenv("TZDIR", previous->c_str(), 1);
        else unsetenv("TZDIR");
    }
};

}  // namespace

TEST_CASE_FIXTURE(ZoneDir, "TZif zones match zoneinfo offsets, including far beyond their transition tables") {
    for (const auto& v : kZoneOffsets) {
        auto zone = TimeZone::find(v.zone);
        REQUIRE(zone);
        CAPTURE(v.zone);
        CAPTURE(v.epoch);
        CHECK(zone->offset_seconds(static_cast<double>(v.epoch)) == v.offset);
    }
}

TEST_CASE_FIXTURE(ZoneDir, "wall-clock times in gaps and overlaps resolve like zoneinfo with fold=0") {
    for (const auto& v : kZoneLocalTimes) {
        auto zone = TimeZone::find(v.zone);
        REQUIRE(zone);
        CAPTURE(v.zone);
        CAPTURE(v.date);
        CAPTURE(v.hour);
        CAPTURE(v.minute);
        auto epoch = zone->epoch(v.date, v.hour, v.minute, v.second);
        REQUIRE(epoch);
        CHECK(static_cast<long long>(*epoch) == v.epoch);
    }
}

TEST_CASE_FIXTURE(ZoneDir, "TZif zones format local times, dates and field suffixes") {
    auto kolkata = TimeZone::find("Asia/Kolkata");
    REQUIRE(kolkata);
    double sample = ng::parse_iso8601("2026-10-21T13:52:51.250000+00:00")->epoch_seconds;
    CHECK(kolkata->iso(sample) == "2026-10-21T19:22:51.250000+05:30");
    CHECK(kolkata->field_suffix() == "kolkata");
    auto chatham = TimeZone::find("Pacific/Chatham");
    REQUIRE(chatham);
    CHECK(chatham->iso(sample) == "2026-10-22T03:37:51.250000+13:45");
    CHECK(chatham->date(sample) == "2026-10-22");
    auto london = TimeZone::find("Europe/London");
    REQUIRE(london);
    CHECK(london->iso(ng::parse_iso8601("2026-07-01T12:00:00Z")->epoch_seconds) == "2026-07-01T13:00:00+01:00");
    CHECK(london->field_suffix() == "london");
}

TEST_CASE_FIXTURE(ZoneDir, "unknown, unsafe and malformed zone files are rejected") {
    CHECK_FALSE(TimeZone::find("Mars/Olympus"));
    CHECK_FALSE(TimeZone::find(""));
    CHECK_FALSE(TimeZone::find("../Europe/London"));
    CHECK_FALSE(TimeZone::find("/etc/passwd"));
    CHECK_FALSE(TimeZone::find("Europe/London\n"));
    std::filesystem::create_directories(dir.path / "Broken");
    ngtest::write_file((dir.path / "Broken" / "Truncated").string(), std::string(reinterpret_cast<const char*>(kTzifFiles[0].data), 60));
    ngtest::write_file((dir.path / "Broken" / "Text").string(), "not a time zone");
    CHECK_FALSE(TimeZone::find("Broken/Truncated"));
    CHECK_FALSE(TimeZone::find("Broken/Text"));
}

TEST_CASE("built-in zones need no tz database and resolve gaps and overlaps like zoneinfo") {
    std::optional<std::string> previous;
    if (const char* old = std::getenv("TZDIR")) previous = old;
    setenv("TZDIR", "/nonexistent-zoneinfo", 1);
    auto ny = TimeZone::find("America/New_York");
    REQUIRE(ny);
    CHECK(TimeZone::find("Asia/Shanghai"));
    // Python: datetime(2026, 3, 8, 2, 30, tzinfo=ZoneInfo("America/New_York")).timestamp()
    CHECK(*ny->epoch("2026-03-08", 2, 30) == 1772955000.0);   // gap: shifted to 03:30 EDT
    CHECK(*ny->epoch("2026-11-01", 1, 30) == 1793511000.0);   // overlap: first occurrence (EDT)
    CHECK(*ny->epoch("2026-11-01", 2, 30) == 1793518200.0);   // after the overlap: EST
    if (previous) setenv("TZDIR", previous->c_str(), 1);
    else unsetenv("TZDIR");
}
