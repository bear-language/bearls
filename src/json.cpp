//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "json.hpp"
#include <cmath>
#include <limits>

namespace json {

std::optional<Value> parse(std::string_view text) {
    Value value = Value::parse(text, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) {
        return std::nullopt;
    }
    return value;
}

std::string dump(const Value& value) {
    return value.dump(-1, ' ', /*ensure_ascii=*/false, Value::error_handler_t::replace);
}

const Value& get(const Value& value, std::string_view key) noexcept {
    static const Value null_value;
    const Value::object_t* object = value.get_ptr<const Value::object_t*>();
    if (!object) {
        return null_value;
    }
    const auto it = object->find(key);
    return it == object->end() ? null_value : it->second;
}

std::optional<bool> as_bool(const Value& value) noexcept {
    const Value::boolean_t* b = value.get_ptr<const Value::boolean_t*>();
    return b ? std::optional<bool>{*b} : std::nullopt;
}

std::optional<int64_t> as_int(const Value& value) noexcept {
    if (const Value::number_integer_t* i = value.get_ptr<const Value::number_integer_t*>()) {
        return static_cast<int64_t>(*i);
    }
    if (const Value::number_unsigned_t* u = value.get_ptr<const Value::number_unsigned_t*>()) {
        if (*u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<int64_t>(*u);
    }
    if (const Value::number_float_t* d = value.get_ptr<const Value::number_float_t*>()) {
        // 2^63 is exactly representable, anything at or past it doesn't fit
        constexpr double LIMIT = 9223372036854775808.0;
        if (std::trunc(*d) != *d || *d < -LIMIT || *d >= LIMIT) {
            return std::nullopt;
        }
        return static_cast<int64_t>(*d);
    }
    return std::nullopt;
}

std::optional<std::string_view> as_string(const Value& value) noexcept {
    const Value::string_t* s = value.get_ptr<const Value::string_t*>();
    return s ? std::optional<std::string_view>{*s} : std::nullopt;
}

const Array* as_array(const Value& value) noexcept {
    return value.get_ptr<const Value::array_t*>();
}

} // namespace json
