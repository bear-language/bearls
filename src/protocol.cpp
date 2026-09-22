//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "protocol.hpp"
#include <limits>
#include <utility>

namespace lsp {

namespace {

std::optional<uint32_t> as_uinteger(const json::Value& value) {
    const std::optional<int64_t> i = json::as_int(value);
    if (!i || *i < 0 || *i > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*i);
}

} // namespace

std::string_view to_string(PositionEncoding enc) noexcept {
    switch (enc) {
    case PositionEncoding::utf8:
        return "utf-8";
    case PositionEncoding::utf16:
        return "utf-16";
    case PositionEncoding::utf32:
        return "utf-32";
    }
    std::unreachable();
}

std::optional<Position> parse_position(const json::Value& value) {
    const std::optional<uint32_t> line = as_uinteger(json::get(value, "line"));
    const std::optional<uint32_t> character = as_uinteger(json::get(value, "character"));
    if (!line || !character) {
        return std::nullopt;
    }
    return Position{.line = *line, .character = *character};
}

std::optional<Range> parse_range(const json::Value& value) {
    const std::optional<Position> start = parse_position(json::get(value, "start"));
    const std::optional<Position> end = parse_position(json::get(value, "end"));
    if (!start || !end) {
        return std::nullopt;
    }
    return Range{.start = *start, .end = *end};
}

void to_json(json::Value& j, Position pos) {
    j = {{"line", pos.line}, {"character", pos.character}};
}

void to_json(json::Value& j, const Range& range) {
    j = {{"start", range.start}, {"end", range.end}};
}

void to_json(json::Value& j, const Location& loc) { j = {{"uri", loc.uri}, {"range", loc.range}}; }

void to_json(json::Value& j, const Diagnostic& diag) {
    j = {
        {"range", diag.range},
        {"severity", static_cast<int32_t>(diag.severity)},
        {"source", "bearc"},
        {"message", diag.message},
    };
    if (!diag.related_information.empty()) {
        json::Value& related = j["relatedInformation"] = json::Value::array();
        for (const DiagnosticRelatedInformation& info : diag.related_information) {
            related.push_back(json::Value{{"location", info.location}, {"message", info.message}});
        }
    }
}

} // namespace lsp
