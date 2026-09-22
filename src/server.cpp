//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "server.hpp"
#include "log.hpp"
#include "project.hpp"
#include "text.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <utility>

#ifndef BEARC_VERSION
#define BEARC_VERSION "unknown"
#endif

namespace lsp {

namespace {

/// TextDocumentSyncKind.Incremental
constexpr int TEXT_DOCUMENT_SYNC_INCREMENTAL = 2;

/// an upper bound on the debounce so a bad setting can't make the server look dead
constexpr int64_t MAX_DEBOUNCE_MS = 10'000;

/// workspace/executeCommand command that jumps to its Location argument
constexpr std::string_view SHOW_LOCATION_COMMAND = "bearls.showLocation";

void read_string_array(const json::Value& value, std::vector<std::string>& out) {
    const json::Array* array = json::as_array(value);
    if (!array) {
        return;
    }
    for (const json::Value& elem : *array) {
        if (const std::optional<std::string_view> s = json::as_string(elem)) {
            out.emplace_back(*s);
        }
    }
}

/// "util.br:3" for a location, the line is one-indexed like an editor shows it
std::string short_location(const Location& loc) {
    const std::string_view uri = loc.uri;
    const size_t slash = uri.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? uri : uri.substr(slash + 1);
    return std::format("{}:{}", name, loc.range.start.line + 1);
}

} // namespace

int Server::run() {
    while (!exit_requested && !connection_lost) {
        // with analysis pending, only wait out the debounce period before catching up
        auto message = transport.read_message(any_stale() ? settings.debounce_ms : -1);
        if (!message) {
            if (message.error() == TransportError::end_of_stream) {
                log::info("client closed the connection");
            } else {
                log::error("{}", to_string(message.error()));
            }
            break;
        }
        if (!message->has_value()) {
            refresh_stale(); // the client went quiet
            continue;
        }
        handle_message(**message);
    }
    return shutdown_requested ? 0 : 1;
}

void Server::handle_message(std::string_view raw) {
    const std::optional<json::Value> parsed = json::parse(raw);
    if (!parsed) {
        constexpr size_t MAX_LOGGED = 200; // a whole document can arrive in one message
        log::error("unparsable message: {}{}", raw.substr(0, MAX_LOGGED),
                   raw.size() > MAX_LOGGED ? "..." : "");
        send_error(json::Value{}, ErrorCode::parse_error, "invalid JSON");
        return;
    }
    const json::Value& message = *parsed;
    const auto id_it = message.find("id"); // end() for anything but an object
    const json::Value* id = id_it == message.end() ? nullptr : &*id_it;
    const std::optional<std::string_view> method = json::as_string(json::get(message, "method"));
    if (!method) {
        // with an id, this is a response to a server -> client request (window/showDocument),
        // which bearls doesn't need
        if (!id) {
            send_error(json::Value{}, ErrorCode::invalid_request, "message has no method");
        }
        return;
    }
    const json::Value& params = json::get(message, "params");
    if (id) {
        handle_request(*id, *method, params);
    } else {
        handle_notification(*method, params);
    }
}

void Server::handle_request(const json::Value& id, std::string_view method,
                            const json::Value& params) {
    static constexpr std::array<std::pair<std::string_view, RequestHandler>, 5> HANDLERS{{
        {"initialize", &Server::on_initialize},
        {"shutdown", &Server::on_shutdown},
        {"textDocument/definition", &Server::on_definition},
        {"textDocument/codeAction", &Server::on_code_action},
        {"workspace/executeCommand", &Server::on_execute_command},
    }};

    if (!initialized && method != "initialize") {
        send_error(id, ErrorCode::server_not_initialized, "server is not initialized yet");
        return;
    }
    if (initialized && method == "initialize") {
        send_error(id, ErrorCode::invalid_request, "server is already initialized");
        return;
    }
    if (shutdown_requested) {
        send_error(id, ErrorCode::invalid_request, "server is shutting down");
        return;
    }
    for (const auto& [name, handler] : HANDLERS) {
        if (name == method) {
            (this->*handler)(id, params);
            return;
        }
    }
    send_error(id, ErrorCode::method_not_found, method);
}

void Server::handle_notification(std::string_view method, const json::Value& params) {
    static constexpr std::array<std::pair<std::string_view, NotificationHandler>, 6> HANDLERS{{
        {"initialized", &Server::on_initialized},
        {"exit", &Server::on_exit},
        {"textDocument/didOpen", &Server::on_did_open},
        {"textDocument/didChange", &Server::on_did_change},
        {"textDocument/didSave", &Server::on_did_save},
        {"textDocument/didClose", &Server::on_did_close},
    }};

    // notifications other than exit are dropped until initialized, unknown ones are ignored
    // (which the spec requires for $/ notifications)
    if (!initialized && method != "exit") {
        return;
    }
    for (const auto& [name, handler] : HANDLERS) {
        if (name == method) {
            (this->*handler)(params);
            return;
        }
    }
}

// ------------------------------------------------------------------------------------------------
// requests
// ------------------------------------------------------------------------------------------------

void Server::on_initialize(const json::Value& id, const json::Value& params) {
    // prefer utf-8 since bearc's spans are byte offsets, utf-16 is the mandatory fallback
    if (const json::Array* encodings
        = json::as_array(json::get(params, "capabilities", "general", "positionEncodings"))) {
        if (std::ranges::any_of(*encodings, [](const json::Value& enc) {
                return json::as_string(enc) == "utf-8";
            })) {
            encoding = PositionEncoding::utf8;
        }
    }

    client_shows_documents
        = json::as_bool(json::get(params, "capabilities", "window", "showDocument", "support"))
              .value_or(false);

    const auto add_folder = [this](std::string_view uri) {
        if (const std::optional<std::string> path = uri_to_path(uri)) {
            workspace_folders.push_back(canonical_path(*path));
        }
    };
    if (const json::Array* folders = json::as_array(json::get(params, "workspaceFolders"))) {
        for (const json::Value& folder : *folders) {
            if (const std::optional<std::string_view> uri
                = json::as_string(json::get(folder, "uri"))) {
                add_folder(*uri);
            }
        }
    }
    if (workspace_folders.empty()) {
        if (const std::optional<std::string_view> uri
            = json::as_string(json::get(params, "rootUri"))) {
            add_folder(*uri);
        }
    }

    const json::Value& options = json::get(params, "initializationOptions");
    read_string_array(json::get(options, "importPaths"), settings.import_paths);
    read_string_array(json::get(options, "extraArgs"), settings.extra_args);
    if (const std::optional<int64_t> ms = json::as_int(json::get(options, "debounceMs"));
        ms && *ms >= 0 && *ms <= MAX_DEBOUNCE_MS) {
        settings.debounce_ms = static_cast<int>(*ms);
    }
    for (std::string& path : settings.import_paths) {
        std::filesystem::path import_path{path};
        if (import_path.is_relative() && !workspace_folders.empty()) {
            import_path = std::filesystem::path{workspace_folders.front()} / import_path;
        }
        path = canonical_path(import_path.string());
    }

    initialized = true;
    send_result(id, {
                        {"capabilities",
                         {
                             {"positionEncoding", to_string(encoding)},
                             {"textDocumentSync",
                              {
                                  {"openClose", true},
                                  {"change", TEXT_DOCUMENT_SYNC_INCREMENTAL},
                                  {"save", {{"includeText", false}}},
                              }},
                             {"definitionProvider", true},
                             {"codeActionProvider", true},
                             {"executeCommandProvider",
                              {{"commands", json::Value::array({SHOW_LOCATION_COMMAND})}}},
                         }},
                        {"serverInfo", {{"name", "bearls"}, {"version", BEARLS_VERSION}}},
                    });
    log::info("initialized (bearls {}, bearc {}, {} positions)", BEARLS_VERSION, BEARC_VERSION,
              to_string(encoding));
}

void Server::on_shutdown(const json::Value& id, const json::Value&) {
    shutdown_requested = true;
    documents.clear(); // nothing will be queried anymore
    projects.clear();
    send_result(id, nullptr);
}

void Server::on_definition(const json::Value& id, const json::Value& params) {
    const std::optional<Position> pos = parse_position(json::get(params, "position"));
    if (!pos) {
        send_error(id, ErrorCode::invalid_params, "missing or malformed position");
        return;
    }
    std::vector<Location> locations;
    if (Document* doc = find_document(params)) {
        ensure_fresh();
        if (Analysis* analysis = analysis_of(*doc)) {
            locations = analysis->definition(doc->path, *pos, encoding);
        }
    }
    send_result(id, locations.empty() ? json::Value(nullptr) : json::Value(locations));
}

void Server::on_code_action(const json::Value& id, const json::Value& params) {
    const std::optional<Range> range = parse_range(json::get(params, "range"));
    if (!range) {
        send_error(id, ErrorCode::invalid_params, "missing or malformed range");
        return;
    }

    struct GoTo {
        std::string title;
        Location target;
        const Diagnostic* diagnostic;
    };
    std::vector<GoTo> actions;
    Document* doc = client_shows_documents ? find_document(params) : nullptr;
    if (doc) {
        ensure_fresh();

        // one action per related location, except where a diagnostic under the cursor already is
        // or where an earlier action already goes
        std::vector<Location> seen;
        for (const Diagnostic& diag : doc->diagnostics) {
            if (touches(diag.range, *range)) {
                seen.push_back(Location{.uri = doc->uri, .range = diag.range});
            }
        }
        for (const Diagnostic& diag : doc->diagnostics) {
            if (!touches(diag.range, *range)) {
                continue;
            }
            for (const DiagnosticRelatedInformation& info : diag.related_information) {
                if (std::ranges::contains(seen, info.location)) {
                    continue;
                }
                seen.push_back(info.location);
                actions.push_back(GoTo{
                    .title
                    = std::format("Go to: {} ({})", info.message, short_location(info.location)),
                    .target = info.location,
                    .diagnostic = &diag,
                });
            }
        }
    }

    json::Value result = json::Value::array();
    for (const GoTo& action : actions) {
        result.push_back({
            {"title", action.title},
            {"kind", "quickfix"},
            {"diagnostics", json::Value::array({*action.diagnostic})},
            {"command",
             {
                 {"title", action.title},
                 {"command", SHOW_LOCATION_COMMAND},
                 {"arguments", json::Value::array({action.target})},
             }},
        });
    }
    send_result(id, std::move(result));
}

void Server::on_execute_command(const json::Value& id, const json::Value& params) {
    if (json::as_string(json::get(params, "command")) != SHOW_LOCATION_COMMAND) {
        send_error(id, ErrorCode::invalid_params, "unknown command");
        return;
    }
    const json::Array* arguments = json::as_array(json::get(params, "arguments"));
    const json::Value* location = arguments && !arguments->empty() ? &arguments->front() : nullptr;
    const std::optional<std::string_view> uri
        = location ? json::as_string(json::get(*location, "uri")) : std::nullopt;
    const std::optional<Range> range
        = location ? parse_range(json::get(*location, "range")) : std::nullopt;
    if (!uri || !range) {
        send_error(id, ErrorCode::invalid_params, "expected a Location argument");
        return;
    }
    send_request("window/showDocument",
                 {{"uri", *uri}, {"takeFocus", true}, {"selection", *range}});
    send_result(id, nullptr);
}

// ------------------------------------------------------------------------------------------------
// notifications
// ------------------------------------------------------------------------------------------------

void Server::on_initialized(const json::Value&) {}

void Server::on_exit(const json::Value&) { exit_requested = true; }

void Server::on_did_open(const json::Value& params) {
    const json::Value& item = json::get(params, "textDocument");
    const std::optional<std::string_view> uri = json::as_string(json::get(item, "uri"));
    const std::optional<std::string_view> text = json::as_string(json::get(item, "text"));
    if (!uri || !text) {
        log::error("ignoring malformed textDocument/didOpen");
        return;
    }
    Document doc{
        .uri = std::string{*uri},
        .path = {},
        .text = std::string{*text},
        .version = json::as_int(json::get(item, "version")).value_or(0),
        .project_file = {},
        .stale = true,
        .in_project = false,
        .analysis = nullptr,
        .diagnostics = {},
    };
    if (const std::optional<std::string> path = uri_to_path(*uri)) {
        doc.path = canonical_path(*path);
    }
    attach_project(doc);
    const std::string path = doc.path;
    std::string key = doc.uri;
    documents.insert_or_assign(std::move(key), std::move(doc));
    // anything that read this file from disk now has to read the buffer instead
    mark_stale(path);
}

void Server::on_did_change(const json::Value& params) {
    Document* doc = find_document(params);
    const json::Array* changes = json::as_array(json::get(params, "contentChanges"));
    if (!doc || !changes) {
        return;
    }
    for (const json::Value& change : *changes) {
        const std::optional<std::string_view> text = json::as_string(json::get(change, "text"));
        if (!text) {
            continue;
        }
        const json::Value& range_value = json::get(change, "range");
        if (range_value.is_null()) {
            doc->text = *text; // full replacement
            continue;
        }
        const std::optional<Range> range = parse_range(range_value);
        if (!range) {
            continue;
        }
        const LineIndex lines{doc->text};
        const size_t start = lines.offset_of(range->start, encoding);
        const size_t end = std::max(start, lines.offset_of(range->end, encoding));
        doc->text.replace(start, end - start, *text);
    }
    doc->version
        = json::as_int(json::get(params, "textDocument", "version")).value_or(doc->version);
    doc->stale = true;
    mark_stale(doc->path);
}

void Server::on_did_save(const json::Value&) {
    // a save is a good moment to pick up changes other tools made to files that aren't open,
    // including .bearls files that were added, removed, or edited
    for (auto& [uri, doc] : documents) {
        doc.stale = true;
        attach_project(doc);
    }
    for (auto& [file, project] : projects) {
        project.stale = true;
    }
    drop_unused_projects();
}

void Server::on_did_close(const json::Value& params) {
    const auto it
        = documents.find(json::as_string(json::get(params, "textDocument", "uri")).value_or(""));
    if (it == documents.end()) {
        return;
    }
    const std::string path = std::move(it->second.path);
    const std::string uri = it->first;
    documents.erase(it);
    drop_unused_projects();
    clear_diagnostics(uri);
    // anything that read this file's buffer now has to read it from disk instead
    mark_stale(path);
}

// ------------------------------------------------------------------------------------------------
// analysis scheduling
// ------------------------------------------------------------------------------------------------

Document* Server::find_document(const json::Value& params) {
    const std::optional<std::string_view> uri
        = json::as_string(json::get(params, "textDocument", "uri"));
    if (!uri) {
        return nullptr;
    }
    const auto it = documents.find(*uri);
    return it == documents.end() ? nullptr : &it->second;
}

void Server::attach_project(Document& doc) {
    const std::optional<std::string> file
        = doc.path.empty() ? std::nullopt : find_project_file(parent_directory(doc.path));
    doc.project_file = file.value_or("");
    if (file && !projects.contains(*file)) {
        projects.emplace(*file,
                         Project{.file = *file, .stale = true, .analysis = nullptr, .error = {}});
    }
}

void Server::drop_unused_projects() {
    std::erase_if(projects, [this](const auto& entry) {
        return std::ranges::none_of(documents, [&entry](const auto& doc_entry) {
            return doc_entry.second.project_file == entry.first;
        });
    });
}

Analysis* Server::analysis_of(Document& doc) {
    if (doc.in_project) {
        const auto it = projects.find(doc.project_file);
        return it == projects.end() ? nullptr : it->second.analysis.get();
    }
    return doc.analysis.get();
}

void Server::mark_stale(std::string_view path) {
    if (path.empty()) {
        return;
    }
    for (auto& [uri, doc] : documents) {
        if (doc.path == path || (doc.analysis && doc.analysis->includes_file(path))) {
            doc.stale = true;
        }
    }
    for (auto& [file, project] : projects) {
        // without an analysis, there's no telling what an invalid .bearls would have read
        if (!project.analysis || project.analysis->includes_file(path)) {
            project.stale = true;
        }
    }
}

bool Server::any_stale() const noexcept {
    return std::ranges::any_of(documents, [](const auto& entry) { return entry.second.stale; })
           || std::ranges::any_of(projects, [](const auto& entry) { return entry.second.stale; });
}

void Server::refresh_stale() {
    const std::vector<hir::SourceOverlay> current_overlays = overlays();
    std::vector<std::string_view> reanalyzed_projects;
    for (auto& [file, project] : projects) {
        if (project.stale) {
            analyze(project, current_overlays);
            reanalyzed_projects.push_back(file);
        }
    }
    for (auto& [uri, doc] : documents) {
        const auto project = projects.find(doc.project_file);
        const bool covered = project != projects.end() && project->second.analysis
                             && project->second.analysis->includes_file(doc.path);
        if (covered) {
            doc.analysis.reset(); // the project's analysis answers for it now
            doc.stale = false;
            const bool newly_covered = !doc.in_project;
            doc.in_project = true;
            if (newly_covered || std::ranges::contains(reanalyzed_projects, doc.project_file)) {
                publish_diagnostics(doc, project->second.analysis.get());
            }
        } else if (doc.stale || doc.in_project) {
            // not (or no longer) reachable from its project, so it stands on its own until it is
            doc.in_project = false;
            analyze(doc, current_overlays);
        }
    }
}

void Server::ensure_fresh() {
    if (any_stale()) {
        refresh_stale();
    }
}

void Server::analyze(Document& doc, std::span<const hir::SourceOverlay> current_overlays) {
    doc.stale = false;
    if (doc.path.empty()) {
        return; // not a file, so bearc can't resolve it (or its imports)
    }
    const auto start = std::chrono::steady_clock::now();
    doc.analysis.reset(); // release the old one first to keep peak memory down
    doc.analysis = std::make_unique<Analysis>(single_file_args(doc.path, analysis_options_for(doc)),
                                              current_overlays);
    const std::chrono::duration<double, std::milli> elapsed
        = std::chrono::steady_clock::now() - start;
    log::verbose("analyzed {} (v{}) in {:.3f}ms", doc.path, doc.version, elapsed.count());
    publish_diagnostics(doc, doc.analysis.get());
}

void Server::analyze(Project& project, std::span<const hir::SourceOverlay> current_overlays) {
    project.stale = false;
    project.analysis.reset();
    std::expected<ProjectConfig, std::string> config = read_project_file(project.file);
    if (!config) {
        // its documents fall back to analyses of their own
        if (config.error() != project.error) {
            project.error = config.error();
            log::error("{}", project.error);
            show_error(project.error);
        }
        return;
    }
    const std::string working_dir
        = project_working_directory(project.file, *config, workspace_folders);
    std::expected<ScopedWorkingDirectory, std::string> in_working_dir
        = ScopedWorkingDirectory::enter(working_dir);
    if (!in_working_dir) {
        const std::string error = std::format("{}: {}", project.file, in_working_dir.error());
        if (error != project.error) {
            project.error = error;
            log::error("{}", project.error);
            show_error(project.error);
        }
        return;
    }
    project.error.clear();
    const auto start = std::chrono::steady_clock::now();
    project.analysis = std::make_unique<Analysis>(std::move(config->args), current_overlays);
    const std::chrono::duration<double, std::milli> elapsed
        = std::chrono::steady_clock::now() - start;
    log::verbose("analyzed project {} (from {}) in {:.3f}ms", project.file, working_dir,
                 elapsed.count());
}

std::vector<hir::SourceOverlay> Server::overlays() const {
    std::vector<hir::SourceOverlay> out;
    out.reserve(documents.size());
    for (const auto& [uri, doc] : documents) {
        if (!doc.path.empty()) {
            out.push_back(hir::SourceOverlay{.path = doc.path, .src = doc.text.c_str()});
        }
    }
    return out;
}

AnalysisOptions Server::analysis_options_for(const Document& doc) const {
    AnalysisOptions options;
    // bearc searches import paths rather than the importing file's directory, so mirror the
    // usual `bearc main.br -I <its dir>` invocation by searching the root's own directory first
    options.import_paths.reserve(1 + settings.import_paths.size() + workspace_folders.size());
    options.import_paths.push_back(parent_directory(doc.path));
    options.import_paths.insert(options.import_paths.end(), settings.import_paths.begin(),
                                settings.import_paths.end());
    options.import_paths.insert(options.import_paths.end(), workspace_folders.begin(),
                                workspace_folders.end());
    options.extra_args = settings.extra_args;
    return options;
}

// ------------------------------------------------------------------------------------------------
// outgoing messages
// ------------------------------------------------------------------------------------------------

void Server::publish_diagnostics(Document& doc, Analysis* analysis) {
    doc.diagnostics
        = analysis ? analysis->diagnostics(doc.path, encoding) : std::vector<Diagnostic>{};
    send_notification("textDocument/publishDiagnostics", {
                                                             {"uri", doc.uri},
                                                             {"version", doc.version},
                                                             {"diagnostics", doc.diagnostics},
                                                         });
}

void Server::show_error(std::string_view message) {
    send_notification("window/showMessage", {{"type", 1}, {"message", message}}); // Error
}

void Server::clear_diagnostics(std::string_view uri) {
    send_notification("textDocument/publishDiagnostics",
                      {{"uri", uri}, {"diagnostics", json::Value::array()}});
}

void Server::send_result(const json::Value& id, json::Value result) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void Server::send_error(const json::Value& id, ErrorCode code, std::string_view message) {
    send({
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", static_cast<int32_t>(code)}, {"message", message}}},
    });
}

void Server::send_notification(std::string_view method, json::Value params) {
    send({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

void Server::send_request(std::string_view method, json::Value params) {
    send({
        {"jsonrpc", "2.0"},
        {"id", next_request_id++},
        {"method", method},
        {"params", std::move(params)},
    });
}

void Server::send(const json::Value& message) {
    const std::string body = json::dump(message);
    if (!transport.write_message(body)) {
        log::error("lost the connection to the client");
        connection_lost = true;
    }
}

} // namespace lsp
