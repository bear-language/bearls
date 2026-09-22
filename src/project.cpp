//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "project.hpp"
#include "cli/args.h"
#include "text.hpp"
#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace lsp {

namespace {

constexpr std::string_view WHITESPACE = " \t\r\n";

std::string_view trim(std::string_view s) noexcept {
    const size_t start = s.find_first_not_of(WHITESPACE);
    if (start == std::string_view::npos) {
        return {};
    }
    return s.substr(start, s.find_last_not_of(WHITESPACE) - start + 1);
}

std::vector<std::string> split_whitespace(std::string_view s) {
    std::vector<std::string> out;
    size_t at = s.find_first_not_of(WHITESPACE);
    while (at != std::string_view::npos) {
        const size_t end = s.find_first_of(WHITESPACE, at);
        out.emplace_back(s.substr(at, end == std::string_view::npos ? end : end - at));
        at = s.find_first_not_of(WHITESPACE, end);
    }
    return out;
}

/// rejects anything bearc's own arg parser would, before it reaches a ContextDatabase (whose
/// Context asserts there is an input file)
std::optional<std::string> invalid_args_reason(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str())); // parse_cli_args doesn't write through
    }
    const bearc_args_t parsed = parse_cli_args(static_cast<int>(argv.size()), argv.data());
    if (parsed.flags[CLI_FLAG_ERR_UNKNOWN]) {
        return "unknown flag";
    }
    if (parsed.flags[CLI_FLAG_ERR_DUPLICATE]) {
        return "duplicate flag";
    }
    if (parsed.flags[CLI_FLAG_ERR_TOO_MANY_INPUT_FILES]) {
        return "more than one input file";
    }
    if (parsed.flags[CLI_FLAG_ERR_FILE_NAME_TOO_LONG]) {
        return "input file name is too long";
    }
    if (parsed.flags[CLI_FLAG_ERR_NO_ARGUMENT_PROVIDED_TO_IMPORT_PATH]
        || parsed.flags[CLI_FLAG_ERR_NO_ARGUMENT_PROVIDED_TO_IMPORT_FILE]
        || parsed.flags[CLI_FLAG_ERR_NO_ARGUMENT_PROVIDED_TO_OUTPUT]) {
        return "a flag is missing its argument";
    }
    if (!parsed.input_file_name) {
        return "no input file";
    }
    return std::nullopt;
}

/// fills args from an `args` value, nullopt on success
std::optional<std::string> read_args(std::string_view project_file, std::string_view value,
                                     std::vector<std::string>& args) {
    std::vector<std::string> file_args = split_whitespace(value);
    args.reserve(1 + file_args.size());
    args.emplace_back("bearc"); // argv[0], skipped by bearc's arg parser
    for (size_t i = 0; i < file_args.size(); ++i) {
        if (i == 0 && std::filesystem::path{file_args[i]}.filename() == "bearc") {
            continue; // the file spelled out the whole command line
        }
        args.push_back(std::move(file_args[i]));
    }
    if (const std::optional<std::string> reason = invalid_args_reason(args)) {
        return std::format("{}: invalid args ({})", project_file, *reason);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> find_project_file(std::string_view dir) {
    namespace fs = std::filesystem;
    std::error_code err;
    for (fs::path curr{dir}; !curr.empty(); curr = curr.parent_path()) {
        const fs::path candidate = curr / PROJECT_FILE_NAME;
        if (fs::is_regular_file(candidate, err)) {
            return canonical_path(candidate.string());
        }
        if (curr == curr.parent_path()) {
            break; // at the file system root
        }
    }
    return std::nullopt;
}

std::expected<ProjectConfig, std::string> read_project_file(std::string_view project_file) {
    std::ifstream in{std::string{project_file}};
    if (!in) {
        return std::unexpected{std::format("{}: can't be read", project_file)};
    }
    std::stringstream contents;
    contents << in.rdbuf();

    std::optional<std::string_view> args_value;
    std::optional<std::string_view> cwd_value;
    std::optional<std::string_view> import_paths_value;
    const std::string text = contents.str();
    std::string_view rest = text;
    for (size_t line_num = 1; !rest.empty(); ++line_num) {
        const size_t newline = rest.find('\n');
        const std::string_view line = trim(rest.substr(0, newline));
        rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const size_t eq = line.find('=');
        const std::string_view key = eq == std::string_view::npos ? line : trim(line.substr(0, eq));
        std::optional<std::string_view>* value = key == "args"           ? &args_value
                                                 : key == "cwd"          ? &cwd_value
                                                 : key == "import-paths" ? &import_paths_value
                                                                         : nullptr;
        if (eq == std::string_view::npos || !value) {
            return std::unexpected{
                std::format("{}:{}: expected `args = <bearc args>`, `cwd = <dir>`, or "
                            "`import-paths = <dirs>`",
                            project_file, line_num)};
        }
        if (*value) {
            return std::unexpected{
                std::format("{}:{}: `{}` is given more than once", project_file, line_num, key)};
        }
        *value = trim(line.substr(eq + 1));
    }

    ProjectConfig config;
    if (import_paths_value) {
        config.import_paths = split_whitespace(*import_paths_value);
    }
    if (args_value) {
        if (std::optional<std::string> error = read_args(project_file, *args_value, config.args)) {
            return std::unexpected{std::move(*error)};
        }
    }

    if (cwd_value) {
        if (cwd_value->empty()) {
            return std::unexpected{std::format("{}: `cwd` is empty", project_file)};
        }
        // operator/ keeps an absolute cwd as is
        const std::filesystem::path cwd
            = std::filesystem::path{project_file}.parent_path() / *cwd_value;
        std::error_code err;
        if (!std::filesystem::is_directory(cwd, err)) {
            return std::unexpected{
                std::format("{}: cwd `{}` is not a directory", project_file, cwd.string())};
        }
        config.cwd = canonical_path(cwd.string());
    }
    return config;
}

std::string project_working_directory(std::string_view project_file, const ProjectConfig& config,
                                      std::span<const std::string> workspace_folders) {
    if (config.cwd) {
        return *config.cwd;
    }
    const std::filesystem::path dir = std::filesystem::path{project_file}.parent_path();
    std::string_view innermost;
    for (const std::string& folder : workspace_folders) {
        // compared component-wise, so /a/bc isn't inside /a/b
        const std::filesystem::path folder_path{folder};
        const bool contains = std::ranges::mismatch(folder_path, dir).in1 == folder_path.end();
        if (contains && folder.size() > innermost.size()) {
            innermost = folder;
        }
    }
    return innermost.empty() ? dir.string() : std::string{innermost};
}

std::expected<ScopedWorkingDirectory, std::string>
ScopedWorkingDirectory::enter(std::string_view dir) {
    std::error_code err;
    std::filesystem::path previous = std::filesystem::current_path(err);
    if (!err) {
        std::filesystem::current_path(dir, err);
    }
    if (err) {
        return std::unexpected{std::format("can't enter {}: {}", dir, err.message())};
    }
    return ScopedWorkingDirectory{std::move(previous)};
}

ScopedWorkingDirectory::ScopedWorkingDirectory(ScopedWorkingDirectory&& other) noexcept
    : previous{std::exchange(other.previous, {})} {}

ScopedWorkingDirectory::~ScopedWorkingDirectory() {
    if (previous.empty()) {
        return;
    }
    std::error_code err;
    std::filesystem::current_path(previous, err); // nothing sensible to do if the old one is gone
}

} // namespace lsp
