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
#include <tuple>
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

constexpr bool same_span(hir::Span a, hir::Span b) noexcept {
    return a.file_id == b.file_id && a.start == b.start && a.len == b.len;
}

/// notes bearc appends to a chain to point at the code that asked for the code containing an error
/// - not in_generic_instantiation_of_type, whose span is the generic's declaration
constexpr bool is_instantiation_note(hir::diag_code code) noexcept {
    return code == hir::diag_code::in_generic_instantiated_here
           || code == hir::diag_code::called_here;
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
    args.emplace_back("--lib"); // cuz for ls purposes warnings about `main` in random files are
                                // gonna get annoying
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

    // hir (semantic) diagnostics, without chains that duplicate one another
    // - ids are copied out since building messages goes back through the Context
    std::vector<hir::DiagnosticId> hir_diagnostic_ids;
    for (const hir::DiagnosticId did : db.unique_diagnostics_for_file(fid)) {
        hir_diagnostic_ids.push_back(did);
    }
    for (const hir::DiagnosticId did : hir_diagnostic_ids) {
        append_diagnostics_for_hir_error(enc, did, out);
    }

    // in source order, where a parser diagnostic sharing a span with a hir diagnostic stays ahead
    // of it (the sort is stable and parser diagnostics were added first) so it's shown on top
    std::ranges::stable_sort(out, [](const Diagnostic& a, const Diagnostic& b) {
        return std::tie(a.range.start.line, a.range.start.character, a.range.end.line,
                        a.range.end.character)
               < std::tie(b.range.start.line, b.range.start.character, b.range.end.line,
                          b.range.end.character);
    });
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

hir::DiagnosticId Analysis::chain_head(hir::DiagnosticId did) const {
    for (size_t i = 0; i < MAX_DIAGNOSTIC_CHAIN_LEN && db.diagnostic(did).prev.has_value(); ++i) {
        did = db.diagnostic(did).prev.as_id();
    }
    return did;
}

hir::DiagnosticId Analysis::chain_anchor(hir::DiagnosticId head) const {
    hir::DiagnosticId anchor = head;
    hir::OptId<hir::DiagnosticId> curr = head;
    for (size_t i = 0; i < MAX_DIAGNOSTIC_CHAIN_LEN && curr.has_value(); ++i) {
        const hir::Diagnostic& member = db.diagnostic(curr.as_id());
        // later notes are further out, the last one is where the user's own code asked for it
        if (is_instantiation_note(member.code) && !member.span.is_generated()
            && !db.file_is_intrinsic(member.span.file_id)) {
            anchor = curr.as_id();
        }
        curr = member.next;
    }
    return anchor;
}

void Analysis::append_diagnostics_for_hir_error(PositionEncoding enc, hir::DiagnosticId did,
                                                std::vector<Diagnostic>& out) {
    const hir::DiagnosticId head = chain_head(did);
    const hir::DiagnosticId anchor = chain_anchor(head);

    if (did == anchor) {
        // the chain's primary diagnostic: the head's message, but reported where the generic was
        // instantiated / the compt function was called (if anywhere), the head is still reported
        // inside the body below
        const hir::Span span = db.diagnostic(anchor).span;
        std::string message = message_for(head);
        if (anchor != head) {
            message += '\n';
            message += message_for(anchor);
        }
        Diagnostic diag{
            .range = line_index(span.file_id).range_of(span.start, span.len, enc),
            .severity = severity_of(db.diagnostic(head).type),
            .message = std::move(message),
            .related_information = {},
            .fixes = {},
        };
        add_chain(head, anchor, enc, diag);
        out.push_back(std::move(diag));
        return;
    }

    const hir::Diagnostic& diag = db.diagnostic(did);
    // anything but a note/help that follows another diagnostic in its chain is reported on its own
    // - this includes a head whose chain is anchored elsewhere, so an error inside a generic /
    // compt function's body shows up both there and where it was instantiated / called
    if (!is_secondary_in_chain(did)) {
        const hir::Span span = diag.span;
        Diagnostic primary{
            .range = line_index(span.file_id).range_of(span.start, span.len, enc),
            .severity = severity_of(diag.type),
            .message = message_for(did),
            .related_information = {},
            .fixes = {},
        };
        add_chain(head, did, enc, primary);
        out.push_back(std::move(primary));
        return;
    }

    // notes are only related information on the chain's primary diagnostic, but a help pointing
    // somewhere else than the diagnostic it belongs to is also reported on its own, so it's visible
    // where it applies (it stays related information / a fix on that diagnostic too)
    if (diag.type != hir::diag_type::help) {
        return;
    }
    // the help belongs to the closest diagnostic before it that's reported in its own right
    hir::DiagnosticId parent = diag.prev.as_id();
    for (size_t i = 0;
         i < MAX_DIAGNOSTIC_CHAIN_LEN && parent != head && is_secondary_in_chain(parent); ++i) {
        parent = db.diagnostic(parent).prev.as_id();
    }
    const hir::DiagnosticId reported_parent = parent == head ? anchor : parent;
    if (same_span(diag.span, db.diagnostic(parent).span)
        || same_span(diag.span, db.diagnostic(reported_parent).span)) {
        return;
    }
    const std::optional<Location> loc = location_of(diag.span, enc);
    if (!loc) {
        return;
    }
    Diagnostic help{
        .range = loc->range,
        .severity = DiagnosticSeverity::hint,
        .message = message_for(did),
        .related_information = {},
        .fixes = {},
    };
    add_fixes(did, *loc, help.message, help.fixes);
    if (std::optional<Location> parent_loc
        = location_of(db.diagnostic(reported_parent).span, enc)) {
        help.related_information.push_back(DiagnosticRelatedInformation{
            .location = *std::move(parent_loc),
            .message = message_for(parent),
        });
    }
    out.push_back(std::move(help));
}

bool Analysis::is_secondary_in_chain(hir::DiagnosticId did) const {
    const hir::Diagnostic& diag = db.diagnostic(did);
    return diag.prev.has_value()
           && (diag.type == hir::diag_type::note || diag.type == hir::diag_type::help);
}

void Analysis::add_chain(hir::DiagnosticId head, hir::DiagnosticId reported_at,
                         PositionEncoding enc, Diagnostic& out) {
    hir::OptId<hir::DiagnosticId> curr = head;
    for (size_t i = 0; i < MAX_DIAGNOSTIC_CHAIN_LEN && curr.has_value(); ++i) {
        const hir::DiagnosticId member = curr.as_id();
        const hir::Span span = db.diagnostic(member).span;
        curr = db.diagnostic(member).next;
        if (member == reported_at) {
            continue;
        }
        std::optional<Location> loc = location_of(span, enc);
        if (!loc) {
            continue;
        }
        std::string message = message_for(member);
        add_fixes(member, *loc, message, out.fixes);
        out.related_information.push_back(DiagnosticRelatedInformation{
            .location = *std::move(loc),
            .message = std::move(message),
        });
    }
}

std::string Analysis::message_for(hir::DiagnosticId did) {
    std::string message = db.message_for_diagnostic(did);
    if (const auto* sc = std::get_if<hir::DiagnosticSubCode>(&db.diagnostic(did).value)) {
        message += " (";
        message += hir::Diagnostic::message_for_code(sc->sub_code);
        message += ')';
    }
    return message;
}

std::vector<std::string> Analysis::fix_texts(hir::DiagnosticId did) {
    const hir::Diagnostic& diag = db.diagnostic(did);
    if (diag.code == hir::diag_code::remove) {
        return {std::string{}};
    }
    if (diag.code != hir::diag_code::replace_with
        && diag.code != hir::diag_code::did_you_mean_colon) {
        return {};
    }
    // copied out, stringifying a type goes back through the Context
    const hir::DiagnosticMessageValue value = diag.message_value;
    if (const auto* d = std::get_if<hir::DiagnosticSymbolAfterMessage>(&value)) {
        return {std::string{db.symbol(d->sid)}};
    }
    if (const auto* d = std::get_if<hir::DiagnosticIdentifierAfterMessage>(&value)) {
        std::string text;
        for (hir::HirSize i = 0; i < d->sid_slice.len(); ++i) {
            if (i != 0) {
                text += "..";
            }
            text += db.symbol(d->sid_slice.get(i));
        }
        return {std::move(text)};
    }
    if (const auto* d = std::get_if<hir::DiagnosticTypeAfterMessage>(&value)) {
        std::vector<std::string> texts{db.type_to_string_as_mentioned(d->tid)};
        if (std::string canonical = db.type_to_string_canonical(d->tid); canonical != texts[0]) {
            texts.push_back(std::move(canonical));
        }
        return texts;
    }
    if (const auto* d = std::get_if<hir::DiagnosticCanonicalTypeAfterMessage>(&value)) {
        return {db.type_to_string_canonical(d->tid)};
    }
    return {};
}

void Analysis::add_fixes(hir::DiagnosticId did, const Location& loc, const std::string& message,
                         std::vector<DiagnosticFix>& out) {
    std::vector<std::string> texts = fix_texts(did);
    for (std::string& text : texts) {
        out.push_back(DiagnosticFix{
            .title = texts.size() == 1 ? message : "replace with `" + text + '`',
            .location = loc,
            .new_text = std::move(text),
        });
    }
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
    for (const auto& [def, maybe_did] :
         {std::pair{&result.variable, ids.variable_id}, std::pair{&result.type, ids.type_id},
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
