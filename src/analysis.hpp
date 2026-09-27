//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_ANALYSIS_HPP
#define BEARLS_ANALYSIS_HPP

#include "compiler/hir/context_database.hpp"
#include "protocol.hpp"
#include "text.hpp"
#include <compiler/hir/indexing.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lsp {

struct AnalysisOptions {
    /// forwarded to bearc as -I, searched in order
    std::vector<std::string> import_paths;
    /// forwarded to bearc verbatim, e.g. "--warn-cyclic-import"
    std::vector<std::string> extra_args;
};

/// a (possibly scoped) identifier found in source text, e.g. `foo..Bar..baz`
struct ScopedIdentifier {
    /// outermost first, ending with the component the cursor is on
    std::vector<std::string> components;
    /// byte range of the component the cursor is on
    size_t start;
    size_t len;
};

/// finds the scoped identifier under a byte offset
/// - components right of the cursor are dropped, so the cursor on `Bar` in `foo..Bar..baz` yields
/// {foo, Bar}
/// - member accesses (`x.field`) yield nullopt since resolving them needs type information
[[nodiscard]] std::optional<ScopedIdentifier> scoped_identifier_at(std::string_view src,
                                                                   size_t offset);

/// `///` doc comment blocks by the (zero-indexed) line they end on, with the markers stripped
/// - a block is a run of consecutive lines that start (after indentation) with `///`, and documents
/// whatever is declared on the line right after it
using DocComments = std::unordered_map<uint32_t, std::string>;

/// finds every doc comment block in a file's source
[[nodiscard]] DocComments scan_doc_comments(std::string_view src);

/// argv for analyzing a single file on its own: `bearc <root> -I <import paths>... <extra args>...`
[[nodiscard]] std::vector<std::string> single_file_args(std::string_view root_path,
                                                        const AnalysisOptions& options);

/// one complete run of bearc's frontend (parsing + sema) rooted at a single file, and the queries
/// the server makes against the result
/// - everything the root transitively imports is analyzed too, and any file with an overlay is
/// read from the overlay instead of from disk
class Analysis {
  public:
    /// args is bearc's argv, starting with "bearc" (which bearc's arg parser skips)
    Analysis(std::vector<std::string> args, std::span<const hir::SourceOverlay> overlays);

    Analysis(const Analysis&) = delete;
    Analysis& operator=(const Analysis&) = delete;

    /// checks whether a file (by canonical path) was part of this analysis
    [[nodiscard]] bool includes_file(std::string_view path) const;

    /// gets the parser and hir diagnostics located in a file
    [[nodiscard]] std::vector<Diagnostic> diagnostics(std::string_view path, PositionEncoding enc);

    /// resolves the identifier at a position to where it's defined
    [[nodiscard]] std::vector<Location> definition(std::string_view path, Position pos,
                                                   PositionEncoding enc);

    /// the doc comments of whatever the identifier at a position resolves to, nullopt if it has
    /// none
    [[nodiscard]] std::optional<Hover> hover(std::string_view path, Position pos,
                                             PositionEncoding enc);

  private:
    /// the identifier at a position and what it resolves to
    struct Resolved {
        ScopedIdentifier ident;
        ContextDatabase::DefQueryResult defs;
    };

    [[nodiscard]] std::optional<Resolved> resolve(hir::FileId fid, Position pos,
                                                  PositionEncoding enc);

    /// where a definition's name is, nullopt if it has no real source location
    [[nodiscard]] std::optional<Location> location_of(const hir::Def& def, PositionEncoding enc);

    /// where a span is, nullopt if it has no real source location
    [[nodiscard]] std::optional<Location> location_of(hir::Span span, PositionEncoding enc);

    /// adds the other diagnostics chained to did (e.g. "previous definition here" notes) to out's
    /// related information, and the edits they suggest to out's fixes
    void add_chain(hir::DiagnosticId did, PositionEncoding enc, Diagnostic& out);

    /// the text a "remove" / "replace with" diagnostic would put in place of its span, nullopt for
    /// any other diagnostic
    [[nodiscard]] std::optional<std::string> fix_text(hir::DiagnosticId did) const;

    /// lazily built line index over a file's source in db
    [[nodiscard]] const LineIndex& line_index(hir::FileId fid);

    /// lazily scanned doc comments of a file's source in db
    [[nodiscard]] const DocComments& doc_comments(hir::FileId fid);

    [[nodiscard]] Diagnostic diagnostic_for_parser_error(hir::FileId fid, PositionEncoding enc,
                                                         const compiler_error_t& err);

    [[nodiscard]] std::optional<Diagnostic>
    maybe_diagnostic_for_hir_error(hir::FileId fid, PositionEncoding enc, hir::DiagnosticId did);

    /// bearc_args keeps pointers into these, so they must be initialized before (and outlive) db
    std::vector<std::string> arg_storage;
    ContextDatabase db;
    /// HirId of a FileId -> index over that file's source (which db owns)
    std::unordered_map<hir::HirId, LineIndex> line_indices;
    /// HirId of a FileId -> doc comments in that file's source
    std::unordered_map<hir::HirId, DocComments> file_doc_comments;
};

} // namespace lsp

#endif // !BEARLS_ANALYSIS_HPP
