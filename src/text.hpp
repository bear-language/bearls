//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_TEXT_HPP
#define BEARLS_TEXT_HPP

#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lsp {

/// maps between byte offsets (what bearc's hir::Span uses) and LSP positions for one text buffer
/// - lines are split on '\n' exactly like bearc's lexer does, a trailing '\r' is not counted as
/// part of the line's content
/// - the text is viewed, not owned, so it must outlive the index
class LineIndex {
  public:
    explicit LineIndex(std::string_view text);

    /// gets the position of a byte offset (clamped to the end of the text)
    [[nodiscard]] Position position_of(size_t offset, PositionEncoding enc) const noexcept;

    /// gets the byte offset of a position, clamping past-the-end lines and characters, per spec
    [[nodiscard]] size_t offset_of(Position pos, PositionEncoding enc) const noexcept;

    /// gets the range covering [offset, offset + len)
    [[nodiscard]] Range range_of(size_t offset, size_t len, PositionEncoding enc) const noexcept;

  private:
    /// the content of a line, excluding its line terminator
    [[nodiscard]] std::string_view line_content(size_t line) const noexcept;

    std::string_view text;
    /// byte offset of the first character of each line
    std::vector<uint32_t> line_starts;
};

/// converts a file:// URI into a file system path, nullopt for other schemes or remote hosts
[[nodiscard]] std::optional<std::string> uri_to_path(std::string_view uri);

/// converts an absolute file system path into a file:// URI
[[nodiscard]] std::string path_to_uri(std::string_view path);

/// canonicalizes a path exactly the way bearc does when resolving files, so that paths from the
/// client and paths from a ContextDatabase compare equal
[[nodiscard]] std::string canonical_path(std::string_view path);

/// the directory containing a path
[[nodiscard]] std::string parent_directory(std::string_view path);

} // namespace lsp

#endif // !BEARLS_TEXT_HPP
