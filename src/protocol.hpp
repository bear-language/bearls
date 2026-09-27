//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_PROTOCOL_HPP
#define BEARLS_PROTOCOL_HPP

#include "json.hpp"
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// the subset of LSP 3.18 structures that bearls speaks
namespace lsp {

/// how Position::character counts (negotiated in initialize, utf-16 is the mandatory fallback)
enum class PositionEncoding : uint8_t { utf8, utf16, utf32 };

[[nodiscard]] std::string_view to_string(PositionEncoding enc) noexcept;

/// zero-indexed, character is in units of the negotiated PositionEncoding
struct Position {
    uint32_t line = 0;
    uint32_t character = 0;

    friend auto operator<=>(const Position&, const Position&) = default;
};

/// end-exclusive
struct Range {
    Position start;
    Position end;

    friend bool operator==(const Range&, const Range&) = default;
};

/// whether a and b share at least one position, touching ends included (a cursor sitting just
/// past a range still counts as on it)
[[nodiscard]] constexpr bool touches(const Range& a, const Range& b) noexcept {
    return a.start <= b.end && b.start <= a.end;
}

struct Location {
    std::string uri;
    Range range;

    friend bool operator==(const Location&, const Location&) = default;
};

enum class DiagnosticSeverity : uint8_t { error = 1, warning = 2, information = 3, hint = 4 };

struct DiagnosticRelatedInformation {
    Location location;
    std::string message;
};

/// an edit suggested by a diagnostic's chain (bearc's "remove ..." / "replace with ..." help)
struct DiagnosticFix {
    std::string title;
    /// the text to replace, an empty new_text deletes it
    Location location;
    std::string new_text;

    friend bool operator==(const DiagnosticFix&, const DiagnosticFix&) = default;
};

struct Diagnostic {
    Range range;
    DiagnosticSeverity severity;
    std::string message;
    std::vector<DiagnosticRelatedInformation> related_information;
    /// not serialized, offered as quickfix code actions instead
    std::vector<DiagnosticFix> fixes;
};

struct Hover {
    /// markdown
    std::string contents;
    /// the identifier hovered over
    Range range;
};

/// JSON-RPC and LSP error codes
enum class ErrorCode : int32_t {
    parse_error = -32700,
    invalid_request = -32600,
    method_not_found = -32601,
    invalid_params = -32602,
    internal_error = -32603,
    server_not_initialized = -32002,
    request_cancelled = -32800,
};

/// reads a {line, character} object
[[nodiscard]] std::optional<Position> parse_position(const json::Value& value);

/// reads a {start, end} object
[[nodiscard]] std::optional<Range> parse_range(const json::Value& value);

// serialization, found by nlohmann::json through ADL, e.g. json::Value{location}
void to_json(json::Value& j, Position pos);
void to_json(json::Value& j, const Range& range);
void to_json(json::Value& j, const Location& loc);
void to_json(json::Value& j, const Diagnostic& diag);
void to_json(json::Value& j, const Hover& hover);

} // namespace lsp

#endif // !BEARLS_PROTOCOL_HPP
