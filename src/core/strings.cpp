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

std::string format_double(double value) {
    if (!std::isfinite(value)) return "null";
    char buf[64];
    auto result = std::to_chars(buf, buf + sizeof(buf), value);
    std::string out(buf, static_cast<std::size_t>(result.ptr - buf));
    // Python prints integral floats as "100.0"; keep JSON-friendly plain form.
    return out;
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
    if (value->is_number_float()) return format_double(value->get<double>());
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
    return !value->empty();
}

std::string json_dump_compact(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string json_dump_sorted(const json& value) {
    // nlohmann::json objects are std::map-backed and therefore already key-sorted.
    return json_dump_compact(value);
}

}  // namespace ng
