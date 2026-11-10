#include "core/strings.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>

#include <openssl/rand.h>

namespace ng {

std::string trim(std::string_view s) {
    std::size_t begin = 0, end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return std::string(s.substr(begin, end - begin));
}

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::vector<std::string> split(std::string_view s, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = s.find(delimiter, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(separator);
        out += parts[i];
    }
    return out;
}

bool truthy_string(std::string_view s) {
    std::string v = to_lower(trim(s));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::string url_encode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            auto hexval = [](char h) {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                return h - 'A' + 10;
            };
            out.push_back(static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2])));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string html_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string canonical_symbol(std::string_view s) {
    std::string symbol = to_upper(trim(s));
    std::size_t slash = symbol.find('/');
    if (slash != std::string::npos) symbol.erase(slash);
    static const std::array<std::string_view, 5> prefixes = {"US.", "HK.", "CN.", "SH.", "SZ."};
    for (auto prefix : prefixes) {
        if (starts_with(symbol, prefix)) return symbol.substr(prefix.size());
    }
    return symbol;
}

std::string normalize_symbol_simple(std::string_view s) { return to_upper(trim(s)); }

std::size_t utf8_length(std::string_view s) {
    std::size_t count = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++count;  // every byte that is not a continuation byte starts a code point
    }
    return count;
}

namespace {

// Decodes one UTF-8 code point starting at `index`; returns its length (1 for an invalid byte).
std::size_t decode_utf8(std::string_view s, std::size_t index, char32_t& code_point) {
    auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    unsigned char lead = byte(index);
    std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
    if (length == 0 || index + length > s.size()) {
        code_point = lead;
        return 1;
    }
    char32_t value = length == 1 ? lead : lead & (0xFF >> (length + 1));
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(index + i) & 0xC0) != 0x80) {
            code_point = lead;
            return 1;
        }
        value = (value << 6) | (byte(index + i) & 0x3F);
    }
    code_point = value;
    return length;
}

bool python_isspace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

}  // namespace

std::string python_strip(std::string_view s) {
    std::size_t begin = 0;
    while (begin < s.size()) {
        char32_t c;
        std::size_t length = decode_utf8(s, begin, c);
        if (!python_isspace(c)) break;
        begin += length;
    }
    // Remember where the last non-space code point ends.
    std::size_t end = begin;
    for (std::size_t index = begin; index < s.size();) {
        char32_t c;
        std::size_t length = decode_utf8(s, index, c);
        index += length;
        if (!python_isspace(c)) end = index;
    }
    return std::string(s.substr(begin, end - begin));
}

bool contains_python_whitespace(std::string_view s) {
    for (std::size_t index = 0; index < s.size();) {
        char32_t c;
        index += decode_utf8(s, index, c);
        if (python_isspace(c)) return true;
    }
    return false;
}

std::string random_token(std::size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) {
        std::random_device rd;
        for (auto& b : buf) b = static_cast<unsigned char>(rd());
    }
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(bytes * 4 / 3 + 4);
    std::size_t i = 0;
    for (; i + 2 < bytes; i += 3) {
        unsigned v = (buf[i] << 16) | (buf[i + 1] << 8) | buf[i + 2];
        out.push_back(alphabet[(v >> 18) & 63]);
        out.push_back(alphabet[(v >> 12) & 63]);
        out.push_back(alphabet[(v >> 6) & 63]);
        out.push_back(alphabet[v & 63]);
    }
    if (i < bytes) {
        unsigned v = buf[i] << 16;
        if (i + 1 < bytes) v |= buf[i + 1] << 8;
        out.push_back(alphabet[(v >> 18) & 63]);
        out.push_back(alphabet[(v >> 12) & 63]);
        if (i + 1 < bytes) out.push_back(alphabet[(v >> 6) & 63]);
    }
    return out;
}

std::string uuid4_hex() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        std::random_device rd;
        for (auto& b : bytes) b = static_cast<unsigned char>(rd());
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

std::string format_double(double value) {
    if (!std::isfinite(value)) return "null";
    char buf[64];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    std::string out(buf, static_cast<std::size_t>(result.ptr - buf));
    // Python prints integral floats as "100.0"; keep JSON-friendly plain form.
    return out;
}

std::string python_float_repr(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";
    char buf[64];
    auto result = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::scientific);  // shortest digits
    std::string_view text(buf, static_cast<std::size_t>(result.ptr - buf));
    bool negative = text.front() == '-';
    if (negative) text.remove_prefix(1);
    std::size_t e = text.find('e');
    std::string digits;
    for (char c : text.substr(0, e)) {
        if (c != '.') digits.push_back(c);
    }
    int exponent = std::atoi(std::string(text.substr(e + 1)).c_str());  // decimal exponent of the first digit
    std::string out = negative ? "-" : "";
    if (exponent < -4 || exponent >= 16) {
        out.push_back(digits[0]);
        if (digits.size() > 1) {
            out.push_back('.');
            out.append(digits, 1, std::string::npos);
        }
        char tail[16];
        std::snprintf(tail, sizeof(tail), "e%c%02d", exponent < 0 ? '-' : '+', std::abs(exponent));
        out += tail;
    } else if (exponent < 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-exponent - 1), '0');
        out += digits;
    } else if (static_cast<int>(digits.size()) <= exponent + 1) {
        out += digits;
        out.append(static_cast<std::size_t>(exponent + 1) - digits.size(), '0');
        out += ".0";
    } else {
        out.append(digits, 0, static_cast<std::size_t>(exponent + 1));
        out.push_back('.');
        out.append(digits, static_cast<std::size_t>(exponent + 1), std::string::npos);
    }
    return out;
}

std::optional<double> python_float(std::string_view raw) {
    std::string text = trim(raw);
    if (text.empty()) return std::nullopt;
    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '+' || text[pos] == '-') {
        negative = text[pos] == '-';
        ++pos;
    }
    std::string body = to_lower(std::string_view(text).substr(pos));
    if (body == "inf" || body == "infinity") return negative ? -INFINITY : INFINITY;
    if (body == "nan") return std::nan("");
    // digitpart ::= digit (["_"] digit)* ; floatnumber ::= [digitpart] "." digitpart | digitpart ["."] [exponent]
    std::string cleaned = negative ? "-" : "";
    auto digitpart = [&](std::size_t& i) {
        std::size_t start = i;
        while (i < body.size()) {
            if (std::isdigit(static_cast<unsigned char>(body[i]))) {
                cleaned.push_back(body[i]);
                ++i;
            } else if (body[i] == '_' && i > start && i + 1 < body.size() &&
                       std::isdigit(static_cast<unsigned char>(body[i - 1])) &&
                       std::isdigit(static_cast<unsigned char>(body[i + 1]))) {
                ++i;
            } else {
                break;
            }
        }
        return i > start;
    };
    std::size_t i = 0;
    bool integer_digits = digitpart(i);
    bool fraction_digits = false;
    if (i < body.size() && body[i] == '.') {
        cleaned.push_back('.');
        ++i;
        fraction_digits = digitpart(i);
    }
    if (!integer_digits && !fraction_digits) return std::nullopt;
    if (i < body.size() && body[i] == 'e') {
        cleaned.push_back('e');
        ++i;
        if (i < body.size() && (body[i] == '+' || body[i] == '-')) cleaned.push_back(body[i++]);
        if (!digitpart(i)) return std::nullopt;
    }
    if (i != body.size()) return std::nullopt;
    char* end = nullptr;
    double value = std::strtod(cleaned.c_str(), &end);
    if (end == cleaned.c_str() || *end != '\0') return std::nullopt;
    return value;
}

}  // namespace ng

// ---------------------------------------------------------------------------
// JSON helpers (declared in core/json.h)
// ---------------------------------------------------------------------------
#include "core/json.h"

namespace ng {

std::optional<double> json_to_double(const json* value) {
    if (!value || value->is_null()) return std::nullopt;
    if (value->is_boolean()) return value->get<bool>() ? 1.0 : 0.0;
    if (value->is_number()) return value->get<double>();
    if (value->is_string()) {
        std::string text = trim(value->get<std::string>());
        if (text.empty()) return std::nullopt;
        std::string lowered = to_lower(text);
        if (lowered == "nan" || lowered == "inf" || lowered == "infinity" || lowered == "-inf" || lowered == "+inf" ||
            lowered == "-infinity" || lowered == "+infinity") {
            if (lowered.find("nan") != std::string::npos) return std::nan("");
            return lowered[0] == '-' ? -INFINITY : INFINITY;
        }
        char* end = nullptr;
        double parsed = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0') return std::nullopt;
        return parsed;
    }
    return std::nullopt;
}

std::optional<double> json_finite_double(const json* value) {
    auto parsed = json_to_double(value);
    if (!parsed || !std::isfinite(*parsed)) return std::nullopt;
    return parsed;
}

std::optional<long long> json_to_int(const json* value) {
    if (!value || value->is_null()) return std::nullopt;
    if (value->is_boolean()) return value->get<bool>() ? 1 : 0;
    if (value->is_number_integer()) return value->get<long long>();
    if (value->is_number_unsigned()) {
        auto u = value->get<unsigned long long>();
        if (u > static_cast<unsigned long long>(LLONG_MAX)) return std::nullopt;
        return static_cast<long long>(u);
    }
    if (value->is_number_float()) {
        double d = value->get<double>();
        if (!std::isfinite(d)) return std::nullopt;
        if (d > 9.2e18 || d < -9.2e18) return std::nullopt;
        return static_cast<long long>(std::trunc(d));
    }
    if (value->is_string()) {
        std::string text = trim(value->get<std::string>());
        if (text.empty()) return std::nullopt;
        // Python int("12") works, int("12.0") does not.
        std::size_t idx = 0;
        if (text[0] == '+' || text[0] == '-') idx = 1;
        if (idx >= text.size()) return std::nullopt;
        for (std::size_t i = idx; i < text.size(); ++i) {
            if (text[i] == '_' && i > idx && i + 1 < text.size()) continue;
            if (!std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
        }
        std::string digits;
        for (char c : text) if (c != '_') digits.push_back(c);
        try {
            return std::stoll(digits);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> json_to_string(const json* value) {
    if (!value || value->is_null()) return std::nullopt;
    if (value->is_string()) return value->get<std::string>();
    if (value->is_boolean()) return value->get<bool>() ? "True" : "False";
    if (value->is_number_integer()) return std::to_string(value->get<long long>());
    if (value->is_number_unsigned()) return std::to_string(value->get<unsigned long long>());
    if (value->is_number_float()) return python_float_repr(value->get<double>());
    return value->dump();
}

bool json_parse_bool(const json* value) {
    if (!value || value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    auto text = json_to_string(value);
    if (!text) return false;
    return truthy_string(*text);
}

bool json_truthy(const json* value) {
    if (!value || value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_number()) return value->get<double>() != 0.0;
    if (value->is_string()) {
        std::string v = to_lower(trim(value->get<std::string>()));
        return v == "1" || v == "true" || v == "yes" || v == "on" || v == "y";
    }
    // Python's _truthy_bool falls back to str(value), which never spells a truthy word for a list or dict.
    return false;
}

bool json_python_truthy(const json* value) {
    if (!value || value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_number()) return value->get<double>() != 0.0;
    if (value->is_string()) return !value->get<std::string>().empty();
    return !value->empty();
}

std::string json_dump_compact(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string json_dump_sorted(const json& value) {
    // nlohmann::json objects are std::map-backed and therefore already key-sorted.
    return json_dump_compact(value);
}

}  // namespace ng
