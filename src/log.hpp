//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_LOG_HPP
#define BEARLS_LOG_HPP

#include <cstdio>
#include <format>
#include <print>
#include <utility>

/// logging goes to stderr, stdout is reserved for the protocol (editors collect stderr into their
/// LSP logs, e.g. :LspLog in neovim)
namespace lsp::log {

/// enables verbose() output, set once at startup
inline bool verbose_enabled = false;

template <typename... Args> void info(std::format_string<Args...> fmt, Args&&... args) {
    std::println(stderr, "[bearls] {}", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void error(std::format_string<Args...> fmt, Args&&... args) {
    std::println(stderr, "[bearls] error: {}", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args> void verbose(std::format_string<Args...> fmt, Args&&... args) {
    if (verbose_enabled) {
        std::println(stderr, "[bearls] {}", std::format(fmt, std::forward<Args>(args)...));
    }
}

} // namespace lsp::log

#endif // !BEARLS_LOG_HPP
