//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "text.hpp"
#include <algorithm>
#include <filesystem>
#include <system_error>

namespace lsp {

namespace {

/// byte length of the UTF-8 sequence introduced by a lead byte (1 for invalid bytes, so that
/// malformed text still advances)
constexpr size_t utf8_sequence_len(unsigned char lead) noexcept {
    if (lead < 0xC0) {
        return 1;
    }
    if (lead < 0xE0) {
        return 2;
    }
    if (lead < 0xF0) {
        return 3;
    }
    if (lead < 0xF8) {
        return 4;
    }
    return 1;
}

/// how many units of an encoding one UTF-8 sequence takes up
constexpr uint32_t units_for_sequence(size_t seq_len, PositionEncoding enc) noexcept {
    switch (enc) {
    case PositionEncoding::utf8:
        return static_cast<uint32_t>(seq_len);
    case PositionEncoding::utf16:
        return seq_len == 4 ? 2 : 1; // astral code points take a surrogate pair
    case PositionEncoding::utf32:
        return 1;
    }
    return 1;
}

constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// RFC 3986 unreserved characters, plus the path separator
constexpr bool is_uri_path_safe(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
           || c == '.' || c == '_' || c == '~' || c == '/';
}

#ifdef _WIN32
/// whether path starts with a drive letter, as in C: or C:/x
constexpr bool is_drive_path(std::string_view path) noexcept {
    return path.size() >= 2 && ((path[0] | 0x20) >= 'a' && (path[0] | 0x20) <= 'z')
           && path[1] == ':' && (path.size() == 2 || path[2] == '/' || path[2] == '\\');
}
#endif

} // namespace

LineIndex::LineIndex(std::string_view text) : text{text} {
    line_starts.push_back(0);
    for (size_t i = text.find('\n'); i != std::string_view::npos; i = text.find('\n', i + 1)) {
        line_starts.push_back(static_cast<uint32_t>(i + 1));
    }
}

std::string_view LineIndex::line_content(size_t line) const noexcept {
    const size_t start = line_starts[line];
    size_t end = (line + 1 < line_starts.size()) ? line_starts[line + 1] - 1 : text.size();
    if (end > start && text[end - 1] == '\r') {
        --end;
    }
    return text.substr(start, end - start);
}

Position LineIndex::position_of(size_t offset, PositionEncoding enc) const noexcept {
    offset = std::min(offset, text.size());
    // last line starting at or before offset
    const auto it = std::ranges::upper_bound(line_starts, offset);
    const auto line = static_cast<size_t>(it - line_starts.begin()) - 1;
    const std::string_view content = line_content(line);
    const size_t col_bytes = std::min(offset - line_starts[line], content.size());

    if (enc == PositionEncoding::utf8) {
        return Position{.line = static_cast<uint32_t>(line),
                        .character = static_cast<uint32_t>(col_bytes)};
    }
    uint32_t units = 0;
    for (size_t i = 0; i < col_bytes;) {
        const size_t seq_len = utf8_sequence_len(static_cast<unsigned char>(content[i]));
        units += units_for_sequence(seq_len, enc);
        i += seq_len;
    }
    return Position{.line = static_cast<uint32_t>(line), .character = units};
}

size_t LineIndex::offset_of(Position pos, PositionEncoding enc) const noexcept {
    if (pos.line >= line_starts.size()) {
        return text.size();
    }
    const std::string_view content = line_content(pos.line);
    const size_t line_start = line_starts[pos.line];

    if (enc == PositionEncoding::utf8) {
        return line_start + std::min<size_t>(pos.character, content.size());
    }
    uint32_t units = 0;
    size_t i = 0;
    while (i < content.size()) {
        const size_t seq_len = utf8_sequence_len(static_cast<unsigned char>(content[i]));
        const uint32_t seq_units = units_for_sequence(seq_len, enc);
        // a position inside a surrogate pair snaps to the start of its code point
        if (units + seq_units > pos.character) {
            break;
        }
        units += seq_units;
        i += seq_len;
    }
    return line_start + std::min(i, content.size());
}

Range LineIndex::range_of(size_t offset, size_t len, PositionEncoding enc) const noexcept {
    return Range{.start = position_of(offset, enc), .end = position_of(offset + len, enc)};
}

std::optional<std::string> uri_to_path(std::string_view uri) {
    static constexpr std::string_view FILE_SCHEME = "file://";
    if (uri.size() < FILE_SCHEME.size()
        || !std::ranges::equal(uri.substr(0, FILE_SCHEME.size()), FILE_SCHEME,
                               [](char a, char b) { return (a | 0x20) == (b | 0x20); })) {
        return std::nullopt;
    }
    uri.remove_prefix(FILE_SCHEME.size());
    // the authority (host) runs up to the path, only local files are supported
    const size_t path_start = uri.find('/');
    if (path_start == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view host = uri.substr(0, path_start);
    std::string path;
    if (!host.empty() && host != "localhost") {
#ifdef _WIN32
        // file://server/share/x is the UNC path \\server\share\x
        path = "//";
        path += host;
#else
        return std::nullopt;
#endif
    }
    std::string_view encoded = uri.substr(path_start);
    // drop any query or fragment
    encoded = encoded.substr(0, encoded.find_first_of("?#"));

    path.reserve(path.size() + encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size() && hex_value(encoded[i + 1]) >= 0
            && hex_value(encoded[i + 2]) >= 0) {
            path += static_cast<char>((hex_value(encoded[i + 1]) << 4) | hex_value(encoded[i + 2]));
            i += 2;
        } else {
            path += encoded[i];
        }
    }
#ifdef _WIN32
    // file:///C:/x (or file:///c%3A/x) is C:\x, not \C:\x
    if (is_drive_path(std::string_view{path}.substr(std::min<size_t>(1, path.size())))) {
        path.erase(0, 1);
    }
    std::ranges::replace(path, '/', '\\');
#endif
    return path;
}

std::string path_to_uri(std::string_view path) {
    static constexpr std::string_view HEX = "0123456789ABCDEF";
    std::string uri = "file://";
    uri.reserve(uri.size() + path.size() + 1);
#ifdef _WIN32
    std::string slashed{path};
    std::ranges::replace(slashed, '\\', '/');
    path = slashed;
    if (path.starts_with("//")) {
        path.remove_prefix(2); // a UNC path's server becomes the URI's host
    } else if (is_drive_path(path)) {
        // C:/x is file:///C:/x, its colon left unescaped as most clients write it
        uri += '/';
        uri += path.substr(0, 2);
        path.remove_prefix(2);
    }
#endif
    for (const char ch : path) {
        const auto c = static_cast<unsigned char>(ch);
        if (is_uri_path_safe(c)) {
            uri += ch;
        } else {
            uri += '%';
            uri += HEX[c >> 4];
            uri += HEX[c & 0xF];
        }
    }
    return uri;
}

std::string canonical_path(std::string_view path) {
    std::error_code err;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, err);
    return err ? std::string{path} : canonical.string();
}

std::string parent_directory(std::string_view path) {
    return std::filesystem::path{path}.parent_path().string();
}

} // namespace lsp
