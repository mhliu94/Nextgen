// nlohmann::json alias and tolerant accessors mirroring the Python helpers.
#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <string_view>

#include <json.hpp>

namespace ng {

using json = nlohmann::json;

// Returns the member if present, else nullptr.
inline const json* json_find(const json& object, std::string_view key) {
    if (!object.is_object()) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

// First present key among `keys` (Python's _first_present / _account_field).
template <typename... Keys>
inline const json* json_first_present(const json& object, Keys... keys) {
    const json* found = nullptr;
    ((found == nullptr ? (found = json_find(object, keys)) : found), ...);
    return found;
}

// float(value) semantics: numbers, numeric strings and bools convert; anything else is nullopt.
std::optional<double> json_to_double(const json* value);
// Same but rejects non-finite results.
std::optional<double> json_finite_double(const json* value);
// int(value) semantics for ints, integral floats, numeric strings; bools convert like Python.
std::optional<long long> json_to_int(const json* value);
// str(value) for strings/numbers/bools; nullopt for null/missing.
std::optional<std::string> json_to_string(const json* value);
// Python truthiness of a JSON value used by the account parser (parse_bool).
bool json_parse_bool(const json* value);
// Python's `bool(value)` truthiness for the fast-trading helpers (_truthy_bool).
bool json_truthy(const json* value);
std::string json_dump_compact(const json& value);
std::string json_dump_sorted(const json& value);

}  // namespace ng
