//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#ifndef BEARLS_TRANSPORT_HPP
#define BEARLS_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lsp {

enum class TransportError : uint8_t {
    end_of_stream,
    io_error,
    malformed_header,
};

[[nodiscard]] std::string_view to_string(TransportError err) noexcept;

/// JSON-RPC base protocol framing (`Content-Length: N\r\n\r\n<body>`) over raw file descriptors
/// - reads use poll(2) so the server can wait for input with a timeout (used for debouncing); on
///   windows, where pipes can't be polled, a reader thread feeds a condition variable instead
/// - this is deliberately not built on iostreams, whose buffering can't be combined with poll(2)
class Transport {
  public:
    Transport(int in_fd, int out_fd) noexcept : in_fd{in_fd}, out_fd{out_fd} {}

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /// waits up to timeout_ms (-1 waits forever) for the next complete message body
    /// - yields std::nullopt when the timeout elapses before any new input arrives
    [[nodiscard]] std::expected<std::optional<std::string>, TransportError>
    read_message(int timeout_ms);

    /// frames and writes one message body, false if the stream is broken
    [[nodiscard]] bool write_message(std::string_view body);

  private:
    /// takes a complete message out of the buffer, if one is there
    [[nodiscard]] std::expected<std::optional<std::string>, TransportError> take_buffered();

    /// reads whatever is available, false if timeout_ms elapsed first
    [[nodiscard]] std::expected<bool, TransportError> fill(int timeout_ms);

    int in_fd;
    int out_fd;
#ifdef _WIN32
    /// windows can't poll pipes, so a background thread does the blocking reads
    struct InputPump;
    std::shared_ptr<InputPump> pump;
#endif
    std::string in_buffer;
    /// start of the unconsumed part of in_buffer
    size_t in_pos = 0;
    std::string out_buffer;
};

} // namespace lsp

#endif // !BEARLS_TRANSPORT_HPP
