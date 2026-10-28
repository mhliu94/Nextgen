// Minimal thread-safe logger with the same line format as the Python servers:
// "<timestamp> <LEVEL> [<name>] <message>". Formatting uses a tiny "{}" substitution
// helper (ng::fmt) so the code builds with GCC 11+ / Clang 14+ without <format>.
#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ng {

namespace detail {
inline void fmt_append(std::ostringstream& out, std::string_view spec, std::string_view& rest) {
    // Appends literal text up to the next "{}" placeholder.
    (void)spec;
    (void)out;
    (void)rest;
}

template <typename T>
inline void fmt_value(std::ostringstream& out, const T& value) {
    out << value;
}
inline void fmt_value(std::ostringstream& out, bool value) { out << (value ? "True" : "False"); }
inline void fmt_value(std::ostringstream& out, std::string_view value) { out.write(value.data(), static_cast<std::streamsize>(value.size())); }
inline void fmt_value(std::ostringstream& out, const std::string& value) { out << value; }
inline void fmt_value(std::ostringstream& out, const char* value) { out << (value ? value : "(null)"); }

inline void fmt_impl(std::ostringstream& out, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '{') { out << '{'; ++i; }
        else if (text[i] == '}' && i + 1 < text.size() && text[i + 1] == '}') { out << '}'; ++i; }
        else out << text[i];
    }
}

template <typename T, typename... Rest>
inline void fmt_impl(std::ostringstream& out, std::string_view text, const T& value, const Rest&... rest) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '{') { out << '{'; ++i; continue; }
        if (text[i] == '}' && i + 1 < text.size() && text[i + 1] == '}') { out << '}'; ++i; continue; }
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '}') {
            fmt_value(out, value);
            fmt_impl(out, text.substr(i + 2), rest...);
            return;
        }
        out << text[i];
    }
}
}  // namespace detail

// Python-style "{}" formatting: fmt("a={} b={}", 1, "x") -> "a=1 b=x".
template <typename... Args>
inline std::string fmt(std::string_view text, const Args&... args) {
    std::ostringstream out;
    out.precision(17);
    detail::fmt_impl(out, text, args...);
    return out.str();
}

namespace log {

enum class Level { Debug = 10, Info = 20, Warning = 30, Error = 40 };

void set_level(Level level);
// Reads LOG_LEVEL (DEBUG/INFO/WARNING/ERROR) from the environment.
void configure_from_env();
Level current_level();
bool enabled(Level level);
void write(Level level, std::string_view name, std::string_view message);

template <typename... Args>
inline void logf(Level level, std::string_view name, std::string_view text, const Args&... args) {
    if (!enabled(level)) return;
    write(level, name, ng::fmt(text, args...));
}

}  // namespace log
}  // namespace ng

#define NG_LOG_DEBUG(name, ...) ::ng::log::logf(::ng::log::Level::Debug, name, __VA_ARGS__)
#define NG_LOG_INFO(name, ...) ::ng::log::logf(::ng::log::Level::Info, name, __VA_ARGS__)
#define NG_LOG_WARN(name, ...) ::ng::log::logf(::ng::log::Level::Warning, name, __VA_ARGS__)
#define NG_LOG_ERROR(name, ...) ::ng::log::logf(::ng::log::Level::Error, name, __VA_ARGS__)
