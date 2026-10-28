#include "core/time_util.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ng {

double wall_now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

double mono_now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t epoch_micros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

namespace {

bool read_digits(std::string_view s, std::size_t& pos, int count, int& out) {
    if (pos + static_cast<std::size_t>(count) > s.size()) return false;
    int value = 0;
    for (int i = 0; i < count; ++i) {
        char c = s[pos + static_cast<std::size_t>(i)];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    pos += static_cast<std::size_t>(count);
    out = value;
    return true;
}

// Days from civil date (Howard Hinnant's algorithm).
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = static_cast<int>(yy + (m <= 2));
}

}  // namespace

std::optional<ParsedTimestamp> parse_iso8601(std::string_view text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    std::string_view s = text.substr(begin, end - begin);
    std::size_t pos = 0;
    int year, month, day;
    if (!read_digits(s, pos, 4, year)) return std::nullopt;
    if (pos >= s.size() || s[pos] != '-') return std::nullopt;
    ++pos;
    if (!read_digits(s, pos, 2, month)) return std::nullopt;
    if (pos >= s.size() || s[pos] != '-') return std::nullopt;
    ++pos;
    if (!read_digits(s, pos, 2, day)) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;

    int hour = 0, minute = 0, second = 0;
    double fraction = 0.0;
    bool has_offset = false;
    int offset_seconds = 0;
    if (pos < s.size()) {
        if (s[pos] != 'T' && s[pos] != 't' && s[pos] != ' ') return std::nullopt;
        ++pos;
        if (!read_digits(s, pos, 2, hour)) return std::nullopt;
        if (pos >= s.size() || s[pos] != ':') return std::nullopt;
        ++pos;
        if (!read_digits(s, pos, 2, minute)) return std::nullopt;
        if (pos < s.size() && s[pos] == ':') {
            ++pos;
            if (!read_digits(s, pos, 2, second)) return std::nullopt;
            if (pos < s.size() && (s[pos] == '.' || s[pos] == ',')) {
                ++pos;
                double scale = 0.1;
                bool any = false;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                    fraction += (s[pos] - '0') * scale;
                    scale /= 10.0;
                    ++pos;
                    any = true;
                }
                if (!any) return std::nullopt;
            }
        }
        if (hour > 23 || minute > 59 || second > 60) return std::nullopt;
        if (pos < s.size()) {
            char c = s[pos];
            if (c == 'Z' || c == 'z') {
                has_offset = true;
                ++pos;
            } else if (c == '+' || c == '-') {
                int sign = c == '-' ? -1 : 1;
                ++pos;
                int oh = 0, om = 0;
                if (!read_digits(s, pos, 2, oh)) return std::nullopt;
                if (pos < s.size() && s[pos] == ':') ++pos;
                if (pos < s.size()) {
                    if (!read_digits(s, pos, 2, om)) return std::nullopt;
                }
                if (oh > 23 || om > 59) return std::nullopt;
                has_offset = true;
                offset_seconds = sign * (oh * 3600 + om * 60);
            } else {
                return std::nullopt;
            }
        }
    }
    if (pos != s.size()) return std::nullopt;
    std::int64_t days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    double epoch = static_cast<double>(days) * 86400.0 + hour * 3600.0 + minute * 60.0 + second + fraction;
    epoch -= offset_seconds;
    return ParsedTimestamp{epoch, has_offset};
}

std::string iso_utc(double epoch_seconds) {
    double whole = std::floor(epoch_seconds);
    long micros = static_cast<long>(std::llround((epoch_seconds - whole) * 1e6));
    if (micros >= 1000000) {
        whole += 1;
        micros -= 1000000;
    }
    std::int64_t secs = static_cast<std::int64_t>(whole);
    std::int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    std::int64_t rem = secs - days * 86400;
    int y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[64];
    if (micros != 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%06ldZ", y, m, d,
                      static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
                      static_cast<long long>(rem % 60), micros);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ", y, m, d,
                      static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
                      static_cast<long long>(rem % 60));
    }
    return buf;
}

std::string iso_utc_now() { return iso_utc(wall_now()); }

std::string filename_timestamp(double epoch_seconds) {
    std::int64_t secs = static_cast<std::int64_t>(std::floor(epoch_seconds));
    std::int64_t days = secs >= 0 ? secs / 86400 : -((-secs + 86399) / 86400);
    std::int64_t rem = secs - days * 86400;
    int y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02u%02uT%02lld%02lld%02lldZ", y, m, d, static_cast<long long>(rem / 3600),
                  static_cast<long long>((rem % 3600) / 60), static_cast<long long>(rem % 60));
    return buf;
}

std::string log_timestamp(double epoch_seconds) {
    std::time_t t = static_cast<std::time_t>(std::floor(epoch_seconds));
    std::tm local{};
    localtime_r(&t, &local);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &local);
    return buf;
}

}  // namespace ng
