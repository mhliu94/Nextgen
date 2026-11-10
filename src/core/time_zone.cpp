#include "core/time_zone.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <vector>

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

bool leap_year(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap_year(y) ? 29 : days[m - 1];
}

}  // namespace

// --- TZif files and their POSIX TZ footer (RFC 8536) ----------------------------------------------

namespace {

// One end of a POSIX daylight-saving rule: "Mm.w.d", "Jn" (1..365, February 29 never counted)
// or "n" (0..365), with an optional "/time" after local midnight.
struct PosixRule {
    char kind = 'M';
    int month = 0, week = 0, weekday = 0, day = 0;
    long time = 7200;
};

struct PosixTz {
    long std_offset = 0;  // seconds east of UTC
    bool has_dst = false;
    long dst_offset = 0;
    PosixRule start, end;
};

class PosixParser {
public:
    explicit PosixParser(const std::string& text) : s_(text) {}

    std::optional<PosixTz> parse() {
        PosixTz tz;
        if (!name()) return std::nullopt;
        auto std_offset = offset(24);
        if (!std_offset) return std::nullopt;
        tz.std_offset = -*std_offset;  // POSIX offsets count west of UTC
        if (at_end()) return tz;
        if (!name()) return std::nullopt;
        tz.has_dst = true;
        tz.dst_offset = tz.std_offset + 3600;
        if (!at_end() && peek() != ',') {
            auto dst_offset = offset(24);
            if (!dst_offset) return std::nullopt;
            tz.dst_offset = -*dst_offset;
        }
        if (at_end()) {
            // No rule given: the POSIX default, which zic never emits; use the current US rule.
            tz.start = PosixRule{'M', 3, 2, 0, 0, 7200};
            tz.end = PosixRule{'M', 11, 1, 0, 0, 7200};
            return tz;
        }
        if (!consume(',') || !rule(tz.start) || !consume(',') || !rule(tz.end) || !at_end()) return std::nullopt;
        return tz;
    }

private:
    bool at_end() const { return pos_ >= s_.size(); }
    char peek() const { return at_end() ? '\0' : s_[pos_]; }
    bool consume(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }
    bool name() {
        if (consume('<')) {
            std::size_t close = s_.find('>', pos_);
            if (close == std::string::npos || close == pos_) return false;
            pos_ = close + 1;
            return true;
        }
        std::size_t begin = pos_;
        while (!at_end() && std::isalpha(static_cast<unsigned char>(peek()))) ++pos_;
        return pos_ - begin >= 3;
    }
    std::optional<long> number(long max) {
        std::size_t begin = pos_;
        long value = 0;
        while (!at_end() && std::isdigit(static_cast<unsigned char>(peek()))) {
            value = value * 10 + (peek() - '0');
            if (value > max) return std::nullopt;
            ++pos_;
        }
        if (pos_ == begin) return std::nullopt;
        return value;
    }
    // [+-]hh[:mm[:ss]] in seconds; hours up to max_hours (167 for rule times in TZif v3+).
    std::optional<long> offset(long max_hours) {
        long sign = 1;
        if (consume('-')) sign = -1;
        else consume('+');
        auto hours = number(max_hours);
        if (!hours) return std::nullopt;
        long total = *hours * 3600;
        if (consume(':')) {
            auto minutes = number(59);
            if (!minutes) return std::nullopt;
            total += *minutes * 60;
            if (consume(':')) {
                auto seconds = number(59);
                if (!seconds) return std::nullopt;
                total += *seconds;
            }
        }
        return sign * total;
    }
    bool rule(PosixRule& out) {
        if (consume('M')) {
            auto month = number(12), week = consume('.') ? number(5) : std::nullopt, weekday = consume('.') ? number(6) : std::nullopt;
            if (!month || !week || !weekday || *month < 1 || *week < 1) return false;
            out = PosixRule{'M', static_cast<int>(*month), static_cast<int>(*week), static_cast<int>(*weekday), 0, 7200};
        } else if (consume('J')) {
            auto day = number(365);
            if (!day || *day < 1) return false;
            out = PosixRule{'J', 0, 0, 0, static_cast<int>(*day), 7200};
        } else {
            auto day = number(365);
            if (!day) return false;
            out = PosixRule{'N', 0, 0, 0, static_cast<int>(*day), 7200};
        }
        if (consume('/')) {
            auto time = offset(167);
            if (!time) return false;
            out.time = *time;
        }
        return true;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

// Days since the epoch of the rule's date in year y.
long long rule_day(const PosixRule& rule, int y) {
    long long jan1 = days_from_civil(y, 1, 1);
    if (rule.kind == 'J') return jan1 + rule.day - 1 + (leap_year(y) && rule.day >= 60 ? 1 : 0);
    if (rule.kind == 'N') return jan1 + rule.day;
    long long first = days_from_civil(y, rule.month, 1);
    int wd = static_cast<int>(((first % 7) + 11) % 7);  // 0 = Sunday
    int day = 1 + (rule.weekday - wd + 7) % 7 + (rule.week - 1) * 7;
    while (day > days_in_month(y, rule.month)) day -= 7;
    return first + day - 1;
}

long posix_offset(const PosixTz& tz, long long t) {
    if (!tz.has_dst) return tz.std_offset;
    long long local_std = t + tz.std_offset;
    long long days = local_std >= 0 ? local_std / 86400 : -((-local_std + 86399) / 86400);
    int y, m, d;
    civil_from_days(days, y, m, d);
    // Transition instants in UTC: the start is given in standard time, the end in daylight time.
    long long start = rule_day(tz.start, y) * 86400 + tz.start.time - tz.std_offset;
    long long end = rule_day(tz.end, y) * 86400 + tz.end.time - tz.dst_offset;
    bool dst = start < end ? (t >= start && t < end) : !(t >= end && t < start);
    return dst ? tz.dst_offset : tz.std_offset;
}

}  // namespace

struct TzifData {
    std::vector<long long> transitions;  // UTC seconds, ascending
    std::vector<unsigned char> types;    // local time type of each transition
    std::vector<long> offsets;           // UT offset of each local time type
    std::optional<PosixTz> footer;

    long offset_at(long long t) const {
        if (transitions.empty() || t < transitions.front()) {
            if (transitions.empty() && footer) return posix_offset(*footer, t);
            return offsets.front();  // RFC 8536: type 0 applies before the first transition
        }
        auto it = std::upper_bound(transitions.begin(), transitions.end(), t);
        if (it == transitions.end() && footer) return posix_offset(*footer, t);
        return offsets[types[static_cast<std::size_t>(it - transitions.begin() - 1)]];
    }
};

namespace {

class Reader {
public:
    explicit Reader(const std::string& data) : d_(data) {}
    bool has(std::size_t n) const { return pos_ + n <= d_.size(); }
    long long be(int bytes) {
        unsigned long long v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | static_cast<unsigned char>(d_[pos_ + static_cast<std::size_t>(i)]);
        pos_ += static_cast<std::size_t>(bytes);
        if (bytes == 4) return static_cast<long long>(static_cast<int32_t>(static_cast<uint32_t>(v)));
        return static_cast<long long>(v);
    }
    void skip(std::size_t n) { pos_ += n; }
    std::size_t pos() const { return pos_; }

private:
    const std::string& d_;
    std::size_t pos_ = 0;
};

std::optional<TzifData> parse_tzif(const std::string& data) {
    Reader r(data);
    struct Counts { long long isut, isstd, leap, time, type, chars; };
    auto header = [&](Counts& c) -> int {
        if (!r.has(44) || data.compare(r.pos(), 4, "TZif") != 0) return -1;
        r.skip(4);
        int version = static_cast<unsigned char>(data[r.pos()]);
        r.skip(16);
        c = {r.be(4), r.be(4), r.be(4), r.be(4), r.be(4), r.be(4)};
        if (c.isut < 0 || c.isstd < 0 || c.leap < 0 || c.time < 0 || c.type < 1 || c.type > 256 || c.chars < 0) return -1;
        return version;
    };
    Counts c{};
    int version = header(c);
    if (version < 0) return std::nullopt;
    int time_size = 4;
    if (version >= '2') {
        // Skip the 32-bit block; the 64-bit one follows with its own header.
        std::size_t v1 = static_cast<std::size_t>(c.time * 5 + c.type * 6 + c.chars + c.leap * 8 + c.isstd + c.isut);
        if (!r.has(v1)) return std::nullopt;
        r.skip(v1);
        if (header(c) < 0) return std::nullopt;
        time_size = 8;
    }
    std::size_t block = static_cast<std::size_t>(c.time * (time_size + 1) + c.type * 6 + c.chars + c.leap * (time_size + 4) + c.isstd + c.isut);
    if (!r.has(block)) return std::nullopt;
    TzifData out;
    for (long long i = 0; i < c.time; ++i) out.transitions.push_back(r.be(time_size));
    for (long long i = 0; i < c.time; ++i) {
        out.types.push_back(static_cast<unsigned char>(r.be(1)));
        if (out.types.back() >= c.type) return std::nullopt;
    }
    for (long long i = 0; i < c.type; ++i) {
        out.offsets.push_back(static_cast<long>(r.be(4)));
        r.skip(2);  // isdst, designation index
    }
    if (!std::is_sorted(out.transitions.begin(), out.transitions.end())) return std::nullopt;
    r.skip(static_cast<std::size_t>(c.chars + c.leap * (time_size + 4) + c.isstd + c.isut));
    if (version >= '2' && r.has(2) && data[r.pos()] == '\n') {
        std::size_t end = data.find('\n', r.pos() + 1);
        if (end == std::string::npos) return std::nullopt;
        std::string footer = data.substr(r.pos() + 1, end - r.pos() - 1);
        if (!footer.empty()) {
            out.footer = PosixParser(footer).parse();
            if (!out.footer) return std::nullopt;
        }
    }
    return out;
}

bool safe_zone_name(const std::string& name) {
    if (name.empty() || name.size() > 255 || name.front() == '/' || name.find("..") != std::string::npos) return false;
    return std::all_of(name.begin(), name.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '/' || ch == '_' || ch == '-' || ch == '+';
    });
}

std::shared_ptr<const TzifData> load_tzif(const std::string& name) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TzifData>> cache;
    if (!safe_zone_name(name)) return nullptr;
    const char* dir = std::getenv("TZDIR");
    std::string path = std::string(dir && *dir ? dir : "/usr/share/zoneinfo") + "/" + name;
    std::lock_guard lock(mutex);
    auto key = path;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::ifstream in(path, std::ios::binary);
    if (!in) return nullptr;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > (1u << 20)) return nullptr;
    auto parsed = parse_tzif(data);
    if (!parsed) return nullptr;
    auto shared = std::make_shared<const TzifData>(std::move(*parsed));
    cache[key] = shared;
    return shared;
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
    if (it != zones.end()) return TimeZone(name, it->second.first, it->second.second);
    if (auto data = load_tzif(name)) return TimeZone(name, Rules::Tzif, 0, std::move(data));
    return std::nullopt;
}

int TimeZone::offset_seconds(double epoch_seconds) const {
    if (rules_ == Rules::UsEastern) return us_eastern_dst(epoch_seconds) ? -4 * 3600 : -5 * 3600;
    if (rules_ == Rules::Tzif) return static_cast<int>(tzif_->offset_at(static_cast<long long>(std::floor(epoch_seconds))));
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
    // Every offset in force within two days of this wall-clock time is a candidate; a candidate
    // is right when it maps back to the same wall-clock time. Several (an overlap): the earliest
    // instant. None (a gap): the offset before the transition, the smallest, which moves the
    // time forward by the gap.
    std::set<int> offsets;
    for (int k = -8; k <= 8; ++k) offsets.insert(offset_seconds(naive + k * 21600.0));
    std::optional<double> best;
    for (int offset : offsets) {
        double candidate = naive - offset;
        if (candidate + offset_seconds(candidate) == naive && (!best || candidate < *best)) best = candidate;
    }
    return best ? *best : naive - *offsets.begin();
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
