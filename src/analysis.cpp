//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "analysis.hpp"
#include "compiler/diagnostics/error_codes.h"
#include "compiler/diagnostics/error_list.h"
#include "compiler/hir/diagnostic.hpp"
#include "utils/vector.h"
#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

namespace lsp {

namespace {

/// guards chain walks against a malformed (cyclic) diagnostic chain
constexpr size_t MAX_DIAGNOSTIC_CHAIN_LEN = 64;

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool is_ident_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_';
}

std::vector<const char*> argv_of(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    return argv;
}

DiagnosticSeverity severity_of(hir::diag_type type) noexcept {
    switch (type) {
    case hir::diag_type::error:
        return DiagnosticSeverity::error;
    case hir::diag_type::warning:
        return DiagnosticSeverity::warning;
    case hir::diag_type::note:
        return DiagnosticSeverity::information;
    case hir::diag_type::help:
        return DiagnosticSeverity::hint;
    }
    return DiagnosticSeverity::error;
}

DiagnosticSeverity severity_of(error_diag_type_e type) noexcept {
    switch (type) {
    case DIAG_TYPE_ERROR:
        return DiagnosticSeverity::error;
    case DIAG_TYPE_WARNING:
        return DiagnosticSeverity::warning;
    case DIAG_TYPE_NOTE:
        return DiagnosticSeverity::information;
    case DIAG_TYPE_HELP:
        return DiagnosticSeverity::hint;
    }
    return DiagnosticSeverity::error;
}

/// byte offset of a pointer into src (e.g. a token's start), clamped to src
size_t offset_in(std::string_view src, const char* p) noexcept {
    const auto base = reinterpret_cast<uintptr_t>(src.data());
    const auto at = reinterpret_cast<uintptr_t>(p);
    return at < base ? 0 : std::min(static_cast<size_t>(at - base), src.size());
}

/// finds name as a whole identifier within src[start, start + len)
std::optional<size_t> find_identifier(std::string_view src, size_t start, size_t len,
                                      std::string_view name) noexcept {
    if (name.empty() || start >= src.size()) {
        return std::nullopt;
    }
    const std::string_view haystack = src.substr(start, len);
    for (size_t at = haystack.find(name); at != std::string_view::npos;
         at = haystack.find(name, at + 1)) {
        const size_t end = at + name.size();
        const bool starts_word = at == 0 || !is_ident_char(haystack[at - 1]);
        const bool ends_word = end == haystack.size() || !is_ident_char(haystack[end]);
        if (starts_word && ends_word) {
            return start + at;
        }
    }
    return std::nullopt;
}

bool same_start(const Location& a, const Location& b) noexcept {
    return a.uri == b.uri && a.range.start.line == b.range.start.line
           && a.range.start.character == b.range.start.character;
}

} // namespace

DocComments scan_doc_comments(std::string_view src) {
    DocComments out;
    std::string block;
    bool in_block = false;
    uint32_t line = 0;
    for (size_t start = 0; start <= src.size(); ++line) {
        const size_t newline = src.find('\n', start);
        const size_t end = newline == std::string_view::npos ? src.size() : newline;
        std::string_view content = src.substr(start, end - start);
        if (content.ends_with('\r')) {
            content.remove_suffix(1);
        }
        const size_t indent = content.find_first_not_of(" \t");
        const std::string_view comment
            = indent == std::string_view::npos ? std::string_view{} : content.substr(indent);
        // four or more slashes is an ordinary comment (e.g. a //////// separator)
        if (comment.starts_with("///") && !comment.starts_with("////")) {
            std::string_view text = comment.substr(3);
            if (text.starts_with(' ')) {
                text.remove_prefix(1);
            }
            if (in_block) {
                block += '\n';
            }
            block += text;
            in_block = true;
        } else if (in_block) {
            out.emplace(line - 1, std::move(block));
            block.clear();
            in_block = false;
        }
        start = end + 1;
    }
    if (in_block) {
        out.emplace(line - 1, std::move(block));
    }
    return out;
}

std::vector<std::string> single_file_args(std::string_view root_path,
                                          const AnalysisOptions& options) {
    std::vector<std::string> args;
    args.reserve(3 + options.import_paths.size() + options.extra_args.size());
    args.emplace_back("bearc"); // argv[0], skipped by bearc's arg parser
    args.emplace_back(root_path);
    if (!options.import_paths.empty()) {
        args.emplace_back("-I");
        args.insert(args.end(), options.import_paths.begin(), options.import_paths.end());
    }
    args.insert(args.end(), options.extra_args.begin(), options.extra_args.end());
    return args;
}

std::optional<ScopedIdentifier> scoped_identifier_at(std::string_view src, size_t offset) {
    offset = std::min(offset, src.size());
    // a cursor just past the end of an identifier still refers to it
    if ((offset == src.size() || !is_ident_char(src[offset])) && offset > 0
        && is_ident_char(src[offset - 1])) {
        --offset;
    }
    if (offset == src.size() || !is_ident_char(src[offset])) {
        return std::nullopt;
    }

    size_t start = offset;
    while (start > 0 && is_ident_char(src[start - 1])) {
        --start;
    }
    size_t end = offset;
    while (end < src.size() && is_ident_char(src[end])) {
        ++end;
    }
    if (is_digit(src[start])) {
        return std::nullopt; // number literal
    }

    ScopedIdentifier ident{
        .components = {std::string{src.substr(start, end - start)}},
        .start = start,
        .len = end - start,
    };

    // walk left across `qualifier..` prefixes
    size_t leftmost = start;
    while (leftmost >= 2 && src[leftmost - 1] == '.' && src[leftmost - 2] == '.') {
        const size_t qual_end = leftmost - 2;
        size_t qual_start = qual_end;
        while (qual_start > 0 && is_ident_char(src[qual_start - 1])) {
            --qual_start;
        }
        if (qual_start == qual_end || is_digit(src[qual_start])) {
            // `..` without an identifier before it isn't scope resolution (e.g. a range `0..n`)
            return ident;
        }
        ident.components.emplace(ident.components.begin(),
                                 src.substr(qual_start, qual_end - qual_start));
        leftmost = qual_start;
    }

    // a lone '.' before the identifier means it's a member access
    if (leftmost >= 1 && src[leftmost - 1] == '.') {
        return std::nullopt;
    }
    return ident;
}

Analysis::Analysis(std::vector<std::string> args, std::span<const hir::SourceOverlay> overlays)
    : arg_storage{std::move(args)}, db{argv_of(arg_storage), overlays} {}

bool Analysis::includes_file(std::string_view path) const {
    return db.file_id_for_path(path).has_value();
}

const LineIndex& Analysis::line_index(hir::FileId fid) {
    return line_indices.try_emplace(fid.raw(), db.file_source(fid)).first->second;
}

const DocComments& Analysis::doc_comments(hir::FileId fid) {
    auto it = file_doc_comments.find(fid.raw());
    if (it == file_doc_comments.end()) {
        it = file_doc_comments.emplace(fid.raw(), scan_doc_comments(db.file_source(fid))).first;
    }
    return it->second;
}

std::vector<Diagnostic> Analysis::diagnostics(std::string_view path, PositionEncoding enc) {
    std::vector<Diagnostic> out;
    const hir::OptId<hir::FileId> maybe_fid = db.file_id_for_path(path);
    if (maybe_fid.empty()) {
        return out;
    }
    const hir::FileId fid = maybe_fid.as_id();

    // parser diagnostics live on the file's ast, independent of the hir::Context
    const compiler_error_list_t& parser_errors = db.parser_diagnostics_for_file(fid);
    const size_t parser_error_count = vector_size(&parser_errors.list_vec);
    out.reserve(parser_error_count);
    for (size_t i = 0; i < parser_error_count; ++i) {
        out.push_back(diagnostic_for_parser_error(
            fid, enc,
            *static_cast<const compiler_error_t*>(vector_at(&parser_errors.list_vec, i))));
    }

    // hir (semantic) diagnostics
    // - ids are copied out since building messages goes back through the Context
    const auto& hir_ids = db.diagnostics_for_file(fid);
    const std::vector<hir::DiagnosticId> hir_diagnostic_ids(hir_ids.begin(), hir_ids.end());
    out.reserve(out.size() + hir_diagnostic_ids.size());
    for (const hir::DiagnosticId did : hir_diagnostic_ids) {
        const std::optional<Diagnostic> maybe_diag = maybe_diagnostic_for_hir_error(fid, enc, did);
        if (!maybe_diag) {
            continue;
        }
        out.push_back(maybe_diag.value());
    }
    return out;
}

[[nodiscard]] Diagnostic Analysis::diagnostic_for_parser_error(hir::FileId fid,
                                                               PositionEncoding enc,
                                                               const compiler_error_t& err) {
    const std::string_view src = db.file_source(fid);
    const LineIndex& lines = line_index(fid);
    size_t start = 0;
    size_t end = 0;
    if (err.start_tkn) {
        const token_t* last = err.end_tkn ? err.end_tkn : err.start_tkn;
        start = offset_in(src, err.start_tkn->start);
        end = std::max(start, offset_in(src, last->start + last->len));
    }
    return Diagnostic{
        .range = lines.range_of(start, end - start, enc),
        .severity = severity_of(error_diagnostic_type(err.error_code)),
        .message = ContextDatabase::message_for_parser_diagnostic(err),
        .related_information = {},
        .fixes = {},
    };
}

[[nodiscard]] std::optional<Diagnostic>
Analysis::maybe_diagnostic_for_hir_error(hir::FileId fid, PositionEncoding enc,
                                         hir::DiagnosticId did) {
    const hir::Span span = db.diagnostic(did).span;
    const hir::diag_type type = db.diagnostic(did).type;
    const LineIndex& lines = line_index(fid);
    // a note/help that follows another diagnostic in its chain (e.g. "previous definition
    // here") is only reported as relatedInformation on the chain's other members
    const bool is_secondary = db.diagnostic(did).prev.has_value()
                              && (type == hir::diag_type::note || type == hir::diag_type::help);
    if (is_secondary) {
        return {};
    }
    Diagnostic diag{
        .range = lines.range_of(span.start, span.len, enc),
        .severity = severity_of(type),
        .message = db.message_for_diagnostic(did),
        .related_information = {},
        .fixes = {},
    };
    add_chain(did, enc, diag);
    return diag;
}

void Analysis::add_chain(hir::DiagnosticId did, PositionEncoding enc, Diagnostic& out) {
    const hir::Diagnostic& diag = db.diagnostic(did);
    if (diag.prev.empty() && diag.next.empty()) {
        return; // not part of a chain
    }

    hir::DiagnosticId head = did;
    for (size_t i = 0; i < MAX_DIAGNOSTIC_CHAIN_LEN && db.diagnostic(head).prev.has_value(); ++i) {
        head = db.diagnostic(head).prev.as_id();
    }

    hir::OptId<hir::DiagnosticId> curr = head;
    for (size_t i = 0; i < MAX_DIAGNOSTIC_CHAIN_LEN && curr.has_value(); ++i) {
        const hir::DiagnosticId member = curr.as_id();
        const hir::Span span = db.diagnostic(member).span;
        curr = db.diagnostic(member).next;
        if (member == did) {
            continue;
        }
        std::optional<Location> loc = location_of(span, enc);
        if (!loc) {
            continue;
        }
        // fix text first, building the message goes back through the Context
        std::optional<std::string> new_text = fix_text(member);
        std::string message = db.message_for_diagnostic(member);
        if (new_text) {
            out.fixes.push_back(DiagnosticFix{
                .title = message,
                .location = *loc,
                .new_text = *std::move(new_text),
            });
        }
        out.related_information.push_back(DiagnosticRelatedInformation{
            .location = *std::move(loc),
            .message = std::move(message),
        });
    }
}

std::optional<std::string> Analysis::fix_text(hir::DiagnosticId did) const {
    const hir::Diagnostic& diag = db.diagnostic(did);
    if (diag.code == hir::diag_code::remove) {
        return std::string{};
    }
    if (diag.code == hir::diag_code::replace_with) {
        // only a symbol is literal source text, anything else (e.g. a type) has no fix
        if (const auto* replacement
            = std::get_if<hir::DiagnosticSymbolAfterMessage>(&diag.message_value)) {
            return std::string{db.symbol(replacement->sid)};
        }
    }
    return std::nullopt;
}

std::optional<Analysis::Resolved> Analysis::resolve(hir::FileId fid, Position pos,
                                                    PositionEncoding enc) {
    const LineIndex& lines = line_index(fid);
    std::optional<ScopedIdentifier> ident
        = scoped_identifier_at(db.file_source(fid), lines.offset_of(pos, enc));
    if (!ident) {
        return std::nullopt;
    }

    // the span only serves to find the innermost scope enclosing the identifier
    hir::Span span = hir::Span::generated();
    span.file_id = fid;
    span.start = static_cast<hir::HirSize>(ident->start);
    span.len = static_cast<hir::HirSize>(ident->len);
    const Position byte_pos = lines.position_of(ident->start, PositionEncoding::utf8);
    span.line = byte_pos.line;
    span.col = byte_pos.character;

    ContextDatabase::DefQueryResult defs = db.query_def(span, ident->components);
    const ContextDatabase::DefIdQueryResult ids = db.query_def_id(span, ident->components);
    return Resolved{.ident = *std::move(ident), .defs = std::move(defs), .ids = ids};
}

std::vector<Location> Analysis::definition(std::string_view path, Position pos,
                                           PositionEncoding enc) {
    const hir::OptId<hir::FileId> maybe_fid = db.file_id_for_path(path);
    if (maybe_fid.empty()) {
        return {};
    }
    const std::optional<Resolved> resolved = resolve(maybe_fid.as_id(), pos, enc);
    if (!resolved) {
        return {};
    }

    // one name can resolve in several namespaces at once, list the most specific first
    const ContextDatabase::DefQueryResult& result = resolved->defs;
    std::vector<Location> locations;
    for (const std::optional<hir::Def>* def : {&result.variable, &result.type, &result.mod}) {
        if (!def->has_value()) {
            continue;
        }
        std::optional<Location> loc = location_of(**def, enc);
        if (loc && std::ranges::none_of(locations, [&](const Location& l) {
                return same_start(l, *loc);
            })) {
            locations.push_back(*std::move(loc));
        }
    }
    return locations;
}

std::optional<Hover> Analysis::hover(std::string_view path, Position pos, PositionEncoding enc) {
    const hir::OptId<hir::FileId> maybe_fid = db.file_id_for_path(path);
    if (maybe_fid.empty()) {
        return std::nullopt;
    }
    const hir::FileId fid = maybe_fid.as_id();
    const std::optional<Resolved> resolved = resolve(fid, pos, enc);
    if (!resolved) {
        return std::nullopt;
    }

    // one section per definition, most specific first: its preview, then its doc comments if any
    const ContextDatabase::DefQueryResult& result = resolved->defs;
    const ContextDatabase::DefIdQueryResult& ids = resolved->ids;
    std::vector<hir::DefId> shown;
    std::string contents;
    for (const auto& [def, maybe_did] : {std::pair{&result.variable, ids.variable_id},
                                         std::pair{&result.type, ids.type_id},
                                         std::pair{&result.mod, ids.mod_id}}) {
        if (!def->has_value() || maybe_did.empty()
            || std::ranges::contains(shown, maybe_did.as_id())) {
            continue;
        }
        const hir::DefId did = maybe_did.as_id();
        shown.push_back(did);
        if (!contents.empty()) {
            contents += "\n\n---\n\n";
        }
        contents += db.pretty_preview_for_def(did, hir::string_preview_mode::ticks_bear);
        if (const std::string* doc = doc_comment_of(**def)) {
            contents += "\n\n";
            contents += *doc;
        }
    }
    if (contents.empty()) {
        return std::nullopt;
    }
    return Hover{
        .contents = std::move(contents),
        .range = line_index(fid).range_of(resolved->ident.start, resolved->ident.len, enc),
    };
}

const std::string* Analysis::doc_comment_of(const hir::Def& def) {
    // a doc comment documents the line right after it, so look for a block ending on the line
    // before the declaration starts
    const hir::Span span = def.span;
    if (span.is_generated()) {
        return nullptr;
    }
    const uint32_t line
        = line_index(span.file_id).position_of(span.start, PositionEncoding::utf8).line;
    if (line == 0) {
        return nullptr;
    }
    const DocComments& comments = doc_comments(span.file_id);
    const auto it = comments.find(line - 1);
    return it == comments.end() ? nullptr : &it->second;
}

std::optional<Location> Analysis::location_of(hir::Span span, PositionEncoding enc) {
    if (span.is_generated() || db.file_is_intrinsic(span.file_id)) {
        return std::nullopt;
    }
    return Location{
        .uri = path_to_uri(db.file_name(span.file_id)),
        .range = line_index(span.file_id).range_of(span.start, span.len, enc),
    };
}

std::optional<Location> Analysis::location_of(const hir::Def& def, PositionEncoding enc) {
    const hir::Span span = def.span;
    if (span.is_generated() || db.file_is_intrinsic(span.file_id)) {
        return std::nullopt;
    }
    // a def's span can cover its whole declaration, so narrow it to the name when possible
    const std::string_view name = db.symbol(def.name);
    const std::optional<size_t> name_start
        = find_identifier(db.file_source(span.file_id), span.start, span.len, name);
    const size_t start = name_start.value_or(span.start);
    const size_t len = name_start ? name.size() : 0;
    return Location{
        .uri = path_to_uri(db.file_name(span.file_id)),
        .range = line_index(span.file_id).range_of(start, len, enc),
    };
}

} // namespace lsp
