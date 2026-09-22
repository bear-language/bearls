//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_PROJECT_HPP
#define BEARLS_PROJECT_HPP

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lsp {

/// name of the project file, which holds the bearc invocation for the whole project
inline constexpr std::string_view PROJECT_FILE_NAME = ".bearls";

/// finds the nearest project file in dir or any of its ancestors
[[nodiscard]] std::optional<std::string> find_project_file(std::string_view dir);

/// what a project file says
struct ProjectConfig {
    /// argv for ContextDatabase, so it always starts with "bearc", empty if the file doesn't say
    /// (then the project has no analysis of its own)
    std::vector<std::string> args;
    /// extra -I paths for the per-document analyses of the project's files, as written (relative
    /// ones are relative to the project's working directory)
    std::vector<std::string> import_paths;
    /// absolute directory to run bearc from, nullopt if the file doesn't say
    std::optional<std::string> cwd;
};

/// reads a project file, one `key = value` per line:
///
///     args = main.br -I lib --warn-cyclic-import
///     cwd = ../..
///     import-paths = lib ../shared
///
/// - blank lines and lines starting with '#' are ignored
/// - args (optional) is split on whitespace and kept exactly as written, relative paths in it are
/// relative to the project's working directory, like bearc's are relative to its cwd (a leading
/// "bearc" is accepted and not doubled)
/// - cwd (optional) is the working directory, relative to the project file's directory, and has to
/// exist
/// - import-paths (optional) is split on whitespace, and adds to initializationOptions.importPaths
/// for documents that get an analysis of their own
/// - errors are human-readable, e.g. for window/showMessage
[[nodiscard]] std::expected<ProjectConfig, std::string>
read_project_file(std::string_view project_file);

/// the directory bearc runs from for a project: the config's cwd, else the innermost workspace
/// folder containing the project file (e.g. the repository root), else the project file's own
/// directory
[[nodiscard]] std::string project_working_directory(std::string_view project_file,
                                                    const ProjectConfig& config,
                                                    std::span<const std::string> workspace_folders);

/// makes a directory the process's working directory until destroyed, then restores the old one
/// - bearc resolves relative paths (the input file, -I dirs) against its cwd while a
/// ContextDatabase is constructed, and stores the absolute paths it resolved to, so a project is
/// analyzed as if bearc were run from the project's working directory
class ScopedWorkingDirectory {
  public:
    [[nodiscard]] static std::expected<ScopedWorkingDirectory, std::string>
    enter(std::string_view dir);

    ScopedWorkingDirectory(ScopedWorkingDirectory&& other) noexcept;
    ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;
    ScopedWorkingDirectory& operator=(ScopedWorkingDirectory&&) = delete;
    ~ScopedWorkingDirectory();

  private:
    explicit ScopedWorkingDirectory(std::filesystem::path previous) noexcept
        : previous{std::move(previous)} {}

    /// empty once moved from
    std::filesystem::path previous;
};

} // namespace lsp

#endif // !BEARLS_PROJECT_HPP
