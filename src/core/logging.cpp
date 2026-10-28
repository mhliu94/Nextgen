#include "core/logging.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "core/strings.h"
#include "core/time_util.h"

namespace ng::log {
namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mutex;

const char* level_name(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

}  // namespace

void set_level(Level level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

Level current_level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

bool enabled(Level level) { return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed); }

void configure_from_env() {
    const char* raw = std::getenv("LOG_LEVEL");
    if (!raw) return;
    std::string value = to_upper(trim(raw));
    if (value == "DEBUG") set_level(Level::Debug);
    else if (value == "INFO") set_level(Level::Info);
    else if (value == "WARNING" || value == "WARN") set_level(Level::Warning);
    else if (value == "ERROR" || value == "CRITICAL") set_level(Level::Error);
}

void write(Level level, std::string_view name, std::string_view message) {
    std::string line = log_timestamp(wall_now());
    line.push_back(' ');
    line += level_name(level);
    line += " [";
    line.append(name.data(), name.size());
    line += "] ";
    line.append(message.data(), message.size());
    line.push_back('\n');
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}  // namespace ng::log
