// English / Simplified Chinese UI strings. The legacy keys are generated from the
// Python server's dictionary so API error messages stay identical; new keys for the
// rewritten front end are appended in i18n.cpp.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace ng::i18n {

inline constexpr const char* kDefaultLang = "en";

bool is_supported(std::string_view lang);
// Query parameter wins, then cookie, then English (resolve_lang).
std::string resolve_lang(std::string_view query_lang, std::string_view cookie_lang);
// Translate; falls back to English, then to the key itself.
std::string t(std::string_view lang, std::string_view key);
// Python str.format-style substitution of "{name}" placeholders.
std::string format_named(std::string_view text, const std::vector<std::pair<std::string, std::string>>& values);
// Complete dictionary for a language as a JSON object (for the browser).
json dictionary(std::string_view lang);
std::vector<std::string> supported_langs();

}  // namespace ng::i18n
