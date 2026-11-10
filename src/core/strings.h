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
// Number of Unicode code points in a UTF-8 string (Python's len(str)); invalid bytes count singly.
std::size_t utf8_length(std::string_view s);
// Python's str.strip() and str.isspace() for UTF-8 text: besides ASCII whitespace they cover the
// Unicode separators (U+0085, U+00A0, U+1680, U+2000-200A, U+2028, U+2029, U+202F, U+205F, U+3000)
// and the control separators U+001C-U+001F.
std::string python_strip(std::string_view s);
bool contains_python_whitespace(std::string_view s);
std::string random_token(std::size_t bytes = 32);
// 32 lowercase hex characters of a random (version 4) UUID, like Python's uuid.uuid4().hex.
std::string uuid4_hex();
// Shortest round-trip digits in whichever of fixed/scientific notation is shorter ("100", "0.01").
std::string format_double(double value);
// Exactly Python's repr(float)/str(float): "100.0", "0.01", "1e+16", "1.5e-05", "nan", "inf".
std::string python_float_repr(double value);
// Python's float(text) for a str argument: surrounding whitespace, optional sign, digits with
// single underscores, optional fraction/exponent, or inf/infinity/nan. nullopt when float() raises.
std::optional<double> python_float(std::string_view text);

}  // namespace ng
