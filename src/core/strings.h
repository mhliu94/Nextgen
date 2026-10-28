// Small string helpers shared across the server.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ng {

std::string trim(std::string_view s);
std::string to_upper(std::string_view s);
std::string to_lower(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::vector<std::string> split(std::string_view s, char delimiter);
std::string join(const std::vector<std::string>& parts, std::string_view separator);
// Python-style truthiness used by the Kafka/HTTP parsers: "1", "true", "yes", "on" (case-insensitive).
bool truthy_string(std::string_view s);
// Percent-encode for use in a query string (RFC 3986 unreserved characters are kept).
std::string url_encode(std::string_view s);
std::string url_decode(std::string_view s);
std::string html_escape(std::string_view s);
// Strips market prefixes (US., HK., CN., SH., SZ.) and anything after '/', upper-cases.
std::string canonical_symbol(std::string_view s);
// Upper-cased, trimmed symbol without prefix stripping (used for configured symbols).
std::string normalize_symbol_simple(std::string_view s);
std::string random_token(std::size_t bytes = 32);
// Format a double the way Python's json.dumps / str() would (shortest round trip).
std::string format_double(double value);

}  // namespace ng
