//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_JSON_HPP
#define BEARLS_JSON_HPP

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

/// JSON for the LSP wire format, on top of nlohmann::json
/// - bearls builds with -fno-exceptions, where anything in nlohmann::json that would throw calls
/// std::abort() instead. So messages from the client are only ever read through the non-throwing
/// accessors below (never at(), get<T>(), or a const operator[] with a missing key), and every
/// parse and dump goes through parse() and dump()
namespace json {

using Value = nlohmann::json;
using Array = Value::array_t;

/// parses one complete JSON text, nullopt if it isn't valid JSON
[[nodiscard]] std::optional<Value> parse(std::string_view text);

/// compact serialization, invalid UTF-8 is replaced with U+FFFD (instead of aborting) so the output
/// is always valid JSON
[[nodiscard]] std::string dump(const Value& value);

/// looks up an object member, null if value isn't an object or the member doesn't exist
[[nodiscard]] const Value& get(const Value& value, std::string_view key) noexcept;

/// looks up a nested member, e.g. get(params, "textDocument", "uri")
template <typename... Keys>
[[nodiscard]] const Value& get(const Value& value, std::string_view key, std::string_view next,
                               Keys... rest) noexcept {
    return get(get(value, key), next, rest...);
}

[[nodiscard]] std::optional<bool> as_bool(const Value& value) noexcept;
/// gets the value as an integer, only if it's a number with no fractional part that fits
[[nodiscard]] std::optional<int64_t> as_int(const Value& value) noexcept;
/// views the value's string, which lives as long as value
[[nodiscard]] std::optional<std::string_view> as_string(const Value& value) noexcept;
[[nodiscard]] const Array* as_array(const Value& value) noexcept;

} // namespace json

#endif // !BEARLS_JSON_HPP
