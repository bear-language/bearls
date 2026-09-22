//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_SERVER_HPP
#define BEARLS_SERVER_HPP

#include "analysis.hpp"
#include "json.hpp"
#include "protocol.hpp"
#include "transport.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lsp {

/// an open text document, the client owns its contents while it's open
struct Document {
    std::string uri;
    /// canonical file system path, empty for non-file URIs (which are never analyzed)
    std::string path;
    std::string text;
    int64_t version = 0;
    /// the nearest .bearls above the document (canonical path), empty if there is none
    std::string project_file;
    /// the text, or the text of something it imports, changed since its own analysis
    bool stale = true;
    /// the document is covered by its project's analysis (so it has no analysis of its own)
    bool in_project = false;
    /// analysis rooted at this document, only while it isn't covered by its project's analysis
    std::unique_ptr<Analysis> analysis;
    /// what was last published for this document, code actions are offered against it
    std::vector<Diagnostic> diagnostics;
};

/// every file reachable from a .bearls file's bearc invocation, analyzed once and shared by all
/// the open documents it covers
struct Project {
    /// canonical path of the .bearls file
    std::string file;
    /// something the analysis read (or the .bearls file itself) may have changed
    bool stale = true;
    /// null while the .bearls file is invalid or has no args
    std::unique_ptr<Analysis> analysis;
    /// the .bearls file's import-paths (canonical), added to Settings::import_paths for its
    /// documents that aren't covered by the analysis, empty while the file is invalid
    std::vector<std::string> import_paths;
    /// the last problem reported with the .bearls file, so each one is only shown once
    std::string error;
};

struct Settings {
    /// extra -I paths, relative ones are resolved against the first workspace folder
    std::vector<std::string> import_paths;
    /// passed to bearc verbatim
    std::vector<std::string> extra_args;
    /// quiet period after an edit before re-analyzing open documents
    int debounce_ms = 30;
};

/// a single-threaded LSP server
/// - bearc's frontend is fast enough that a full re-analysis per (debounced) edit beats any
/// incremental bookkeeping, so each open document owns a complete Analysis rooted at itself
class Server {
  public:
    explicit Server(Transport& transport) noexcept : transport{transport} {}

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// serves until the client sends `exit` or the stream closes, returns the process exit code
    [[nodiscard]] int run();

  private:
    using RequestHandler = void (Server::*)(const json::Value& id, const json::Value& params);
    using NotificationHandler = void (Server::*)(const json::Value& params);

    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] size_t operator()(std::string_view s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    void handle_message(std::string_view raw);
    void handle_request(const json::Value& id, std::string_view method, const json::Value& params);
    void handle_notification(std::string_view method, const json::Value& params);

    // requests
    void on_initialize(const json::Value& id, const json::Value& params);
    void on_shutdown(const json::Value& id, const json::Value& params);
    void on_definition(const json::Value& id, const json::Value& params);
    void on_code_action(const json::Value& id, const json::Value& params);
    void on_execute_command(const json::Value& id, const json::Value& params);

    // notifications
    void on_initialized(const json::Value& params);
    void on_exit(const json::Value& params);
    void on_did_open(const json::Value& params);
    void on_did_change(const json::Value& params);
    void on_did_save(const json::Value& params);
    void on_did_close(const json::Value& params);

    // analysis scheduling
    [[nodiscard]] Document* find_document(const json::Value& params);
    /// finds the document's .bearls file (if any) and makes sure its project exists
    void attach_project(Document& doc);
    /// drops projects no open document belongs to anymore
    void drop_unused_projects();
    /// the analysis that answers queries about a document, null if it has none
    [[nodiscard]] Analysis* analysis_of(Document& doc);
    /// marks every document and project that is, or whose analysis read, the file at path
    void mark_stale(std::string_view path);
    [[nodiscard]] bool any_stale() const noexcept;
    /// re-analyzes every stale project and document and publishes their diagnostics
    void refresh_stale();
    /// brings everything up to date before a request, so positions line up with the current text
    void ensure_fresh();
    void analyze(Document& doc, std::span<const hir::SourceOverlay> overlays);
    void analyze(Project& project, std::span<const hir::SourceOverlay> overlays);
    [[nodiscard]] std::vector<hir::SourceOverlay> overlays() const;
    [[nodiscard]] AnalysisOptions analysis_options_for(const Document& doc) const;

    // outgoing messages
    void publish_diagnostics(Document& doc, Analysis* analysis);
    void show_error(std::string_view message);
    void clear_diagnostics(std::string_view uri);
    void send_result(const json::Value& id, json::Value result);
    void send_error(const json::Value& id, ErrorCode code, std::string_view message);
    void send_notification(std::string_view method, json::Value params);
    /// a server -> client request, its response is ignored
    void send_request(std::string_view method, json::Value params);
    /// serializes and sends a whole message
    void send(const json::Value& message);

    Transport& transport;
    std::unordered_map<std::string, Document, StringHash, std::equal_to<>> documents;
    /// by .bearls path
    std::unordered_map<std::string, Project, StringHash, std::equal_to<>> projects;
    Settings settings;
    std::vector<std::string> workspace_folders;
    PositionEncoding encoding = PositionEncoding::utf16;
    /// the client handles window/showDocument, which jumping to related information needs
    bool client_shows_documents = false;
    int64_t next_request_id = 1;
    bool initialized = false;
    bool shutdown_requested = false;
    bool exit_requested = false;
    /// set when the output stream breaks, there is nobody left to serve
    bool connection_lost = false;
};

} // namespace lsp

#endif // !BEARLS_SERVER_HPP
