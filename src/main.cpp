//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "log.hpp"
#include "server.hpp"
#include "transport.hpp"
#include <csignal>
#include <cstdio>
#include <print>
#include <span>
#include <string_view>
#include <unistd.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef BEARC_VERSION
#define BEARC_VERSION "unknown"
#endif

namespace {

constexpr std::string_view USAGE = R"(usage: bearls [options]

the bear language server, speaks LSP over stdin/stdout

options:
  --stdio        use stdin/stdout for the protocol (the default and only transport)
  --verbose      log every analysis and its timing to stderr
  -V, --version  print version info
  -h, --help     print this message
)";

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<size_t>(argc)};
    for (const std::string_view arg : args.empty() ? args : args.subspan(1)) {
        if (arg == "-h" || arg == "--help") {
            std::print("{}", USAGE);
            return 0;
        }
        if (arg == "-V" || arg == "--version") {
            std::println("bearls {} (bearc {})", BEARLS_VERSION, BEARC_VERSION);
            return 0;
        }
        if (arg == "--verbose") {
            lsp::log::verbose_enabled = true;
        } else if (arg != "--stdio") {
            std::print(stderr, "bearls: unknown option '{}'\n\n{}", arg, USAGE);
            return 2;
        }
    }

    // keep a private handle on the real stdout for the protocol, then point fd 1 at stderr so any
    // stray output (e.g. from libbearc) lands in the client's log instead of corrupting the stream
    const int protocol_fd = ::dup(STDOUT_FILENO);
    if (protocol_fd < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        std::perror("bearls: failed to set up stdout");
        return 1;
    }
#ifdef _WIN32
    // text mode would turn the protocol's \r\n into \r\r\n on write and \n on read
    ::_setmode(STDIN_FILENO, _O_BINARY);
    ::_setmode(protocol_fd, _O_BINARY);
#else
    // a vanished client should surface as a failed write, not kill the process
    std::signal(SIGPIPE, SIG_IGN);
#endif

    lsp::Transport transport{STDIN_FILENO, protocol_fd};
    lsp::Server server{transport};
    return server.run();
}
