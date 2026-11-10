// Civil-time support without <chrono> zones (GCC 11 has none).
//
// The zones KTrader uses are built in and work without the system tz database: America/New_York
// (US daylight-saving rules since 2007), UTC, Asia/Shanghai, Asia/Hong_Kong, Asia/Singapore
// (fixed +08:00) and Asia/Tokyo (+09:00). Any other IANA name, as Python's zoneinfo accepts, is
// read from the system tz database ($TZDIR, else /usr/share/zoneinfo) when it is installed:
// TZif transitions, then the file's POSIX TZ rule for later instants.
#pragma once

#include <memory>
#include <optional>
#include <string>

namespace ng {

struct CivilTime {
    int year = 1970, month = 1, day = 1;
    int hour = 0, minute = 0, second = 0;
    int microsecond = 0;
    int offset_seconds = 0;  // local - UTC
};

struct TzifData;

class TimeZone {
public:
    // Returns nullopt for unknown names.
    static std::optional<TimeZone> find(const std::string& name);
    const std::string& name() const { return name_; }
    int offset_seconds(double epoch_seconds) const;
    CivilTime civil(double epoch_seconds) const;
    // "YYYY-MM-DD" of the local date.
    std::string date(double epoch_seconds) const;
    // ISO 8601 local time with the numeric offset ("2026-10-21T09:52:51-04:00"); microseconds
    // are included only when nonzero.
    std::string iso(double epoch_seconds) const;
    // Epoch seconds of a local wall-clock time ("YYYY-MM-DD" + hour/minute), like Python with
    // fold=0: during an overlap the first occurrence is returned, inside a gap the time is
    // shifted forward by the gap.
    std::optional<double> epoch(const std::string& date, int hour, int minute, int second = 0) const;
    // Short suffix used for localized timestamp fields ("new_york", "utc", "shanghai", ...).
    std::string field_suffix() const;

private:
    enum class Rules { Fixed, UsEastern, Tzif };
    TimeZone(std::string name, Rules rules, int fixed_offset, std::shared_ptr<const TzifData> tzif = nullptr)
        : name_(std::move(name)), rules_(rules), fixed_offset_(fixed_offset), tzif_(std::move(tzif)) {}
    std::string name_;
    Rules rules_;
    int fixed_offset_;
    std::shared_ptr<const TzifData> tzif_;
};

// Days since 1970-01-01 for a proleptic Gregorian date (negative before the epoch).
long long days_from_civil(int year, int month, int day);
void civil_from_days(long long days, int& year, int& month, int& day);
// Parses "YYYY-MM-DD"; false when malformed or impossible.
bool parse_civil_date(const std::string& text, int& year, int& month, int& day);

}  // namespace ng
