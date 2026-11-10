// Clock helpers and ISO-8601 parsing/formatting matching the Python server's conventions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ng {

// Seconds since the Unix epoch (UTC), with sub-second precision.
double wall_now();
// Monotonic seconds (steady clock), for timers and spacing.
double mono_now();

struct ParsedTimestamp {
    double epoch_seconds = 0.0;  // interpreted as UTC when no offset is present
    bool has_offset = false;
};

// Accepts ISO-8601 dates/datetimes: "2026-10-23T19:59:58Z", "2026-10-23T19:59:58.123+00:00",
// "2026-10-23 19:59:58", "2026-10-23". A trailing 'Z', "+HH:MM", "+HHMM" or "+HH" offset
// is honoured. Returns nullopt for anything else.
std::optional<ParsedTimestamp> parse_iso8601(std::string_view text);

// Formats like Python's datetime.isoformat() for UTC with a trailing 'Z':
// "2026-10-23T19:59:58Z" or "2026-10-23T19:59:58.123456Z" when microseconds are non-zero.
std::string iso_utc(double epoch_seconds);
std::string iso_utc_now();
// Python's datetime.astimezone(timezone.utc).isoformat(): like iso_utc but with "+00:00".
std::string iso_utc_offset(double epoch_seconds);
// "%Y%m%dT%H%M%SZ" for file names.
std::string filename_timestamp(double epoch_seconds);
// Local-time log timestamp "%Y-%m-%dT%H:%M:%S%z".
std::string log_timestamp(double epoch_seconds);
// Microseconds since the epoch as an integer (command id suffixes).
std::int64_t epoch_micros();

}  // namespace ng
