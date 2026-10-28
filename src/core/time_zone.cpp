#include "core/time_zone.h"

#include <cmath>
#include <cstdio>
#include <map>

namespace ng {

long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

void civil_from_days(long long z, int& year, int& month, int& day) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    year = static_cast<int>(y + (month <= 2));
}

bool parse_civil_date(const std::string& text, int& year, int& month, int& day) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (text[i] < '0' || text[i] > '9') return false;
    }
    year = std::stoi(text.substr(0, 4));
    month = std::stoi(text.substr(5, 2));
    day = std::stoi(text.substr(8, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    int y, m, d;
    civil_from_days(days_from_civil(year, month, day), y, m, d);
    return y == year && m == month && d == day;
}

namespace {

// Weekday 0 = Sunday for a days-since-epoch value (1970-01-01 was a Thursday).
int weekday(long long days) { return static_cast<int>(((days % 7) + 11) % 7); }

// Epoch seconds (UTC) of the nth Sunday (1-based) of a month at the given UTC hour.
double nth_sunday_utc(int year, int month, int n, int utc_hour) {
    long long first = days_from_civil(year, month, 1);
    int wd = weekday(first);
    long long day = first + ((7 - wd) % 7) + 7LL * (n - 1);
    return static_cast<double>(day) * 86400.0 + utc_hour * 3600.0;
}

bool us_eastern_dst(double epoch) {
    long long days = static_cast<long long>(std::floor(epoch / 86400.0));
    int y, m, d;
    civil_from_days(days, y, m, d);
    // Since 2007: second Sunday of March 02:00 EST (07:00 UTC) .. first Sunday of November 02:00 EDT (06:00 UTC).
    double start = nth_sunday_utc(y, 3, 2, 7);
    double end = nth_sunday_utc(y, 11, 1, 6);
    return epoch >= start && epoch < end;
}

}  // namespace

std::optional<TimeZone> TimeZone::find(const std::string& name) {
    static const std::map<std::string, std::pair<Rules, int>> zones = {
        {"America/New_York", {Rules::UsEastern, -5 * 3600}}, {"US/Eastern", {Rules::UsEastern, -5 * 3600}},
        {"UTC", {Rules::Fixed, 0}},                           {"Etc/UTC", {Rules::Fixed, 0}},
        {"Asia/Shanghai", {Rules::Fixed, 8 * 3600}},          {"Asia/Hong_Kong", {Rules::Fixed, 8 * 3600}},
        {"Asia/Singapore", {Rules::Fixed, 8 * 3600}},         {"Asia/Tokyo", {Rules::Fixed, 9 * 3600}},
    };
    auto it = zones.find(name);
    if (it == zones.end()) return std::nullopt;
    return TimeZone(name, it->second.first, it->second.second);
}

int TimeZone::offset_seconds(double epoch_seconds) const {
    if (rules_ == Rules::UsEastern) return us_eastern_dst(epoch_seconds) ? -4 * 3600 : -5 * 3600;
    return fixed_offset_;
}

CivilTime TimeZone::civil(double epoch_seconds) const {
    CivilTime c;
    c.offset_seconds = offset_seconds(epoch_seconds);
    double local = epoch_seconds + c.offset_seconds;
    double whole = std::floor(local);
    c.microsecond = static_cast<int>(std::llround((local - whole) * 1e6));
    if (c.microsecond >= 1000000) {
        whole += 1;
        c.microsecond -= 1000000;
    }
    long long secs = static_cast<long long>(whole);
    long long days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    long long rem = secs - days * 86400;
    civil_from_days(days, c.year, c.month, c.day);
    c.hour = static_cast<int>(rem / 3600);
    c.minute = static_cast<int>((rem % 3600) / 60);
    c.second = static_cast<int>(rem % 60);
    return c;
}

std::string TimeZone::date(double epoch_seconds) const {
    CivilTime c = civil(epoch_seconds);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", c.year, c.month, c.day);
    return buf;
}

std::string TimeZone::iso(double epoch_seconds) const {
    CivilTime c = civil(epoch_seconds);
    int off = c.offset_seconds;
    char sign = off < 0 ? '-' : '+';
    off = off < 0 ? -off : off;
    char buf[64];
    if (c.microsecond != 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06d%c%02d:%02d", c.year, c.month, c.day, c.hour, c.minute,
                      c.second, c.microsecond, sign, off / 3600, (off % 3600) / 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", c.year, c.month, c.day, c.hour, c.minute, c.second,
                      sign, off / 3600, (off % 3600) / 60);
    }
    return buf;
}

std::optional<double> TimeZone::epoch(const std::string& date, int hour, int minute, int second) const {
    int y, m, d;
    if (!parse_civil_date(date, y, m, d) || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
    double naive = static_cast<double>(days_from_civil(y, m, d)) * 86400.0 + hour * 3600.0 + minute * 60.0 + second;
    // Resolve the offset with the local time itself: try the standard offset first, then the
    // offset in force at that instant (handles DST transitions without a tz database).
    double guess = naive - offset_seconds(naive - fixed_offset_);
    double resolved = naive - offset_seconds(guess);
    return resolved;
}

std::string TimeZone::field_suffix() const {
    if (rules_ == Rules::UsEastern) return "new_york";
    if (name_ == "UTC" || name_ == "Etc/UTC") return "utc";
    std::string suffix;
    auto slash = name_.rfind('/');
    std::string city = slash == std::string::npos ? name_ : name_.substr(slash + 1);
    for (char ch : city) suffix.push_back(ch == '_' || ch == '-' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return suffix;
}

}  // namespace ng
