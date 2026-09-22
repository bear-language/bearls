//     /                              /
//    /                              /
//   /_____  _____  _____  _____    /  _____   _  _  _____
//  /     / /____  /____/ /____/   /  /____/  /\  / /  __
// /_____/ /____  /    / /   \    /  /    /  /  \/ /____/
// Copyright (C) 2025-2026 Zachary Mahan

#include "transport.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace lsp {

namespace {

constexpr std::string_view HEADER_TERMINATOR = "\r\n\r\n";
constexpr std::string_view LINE_TERMINATOR = "\r\n";
/// a header block this large without a terminator can only be garbage
constexpr size_t MAX_HEADER_BYTES = 0x2000;
constexpr size_t READ_CHUNK_BYTES = 0x10000;

bool equals_ignoring_case(std::string_view a, std::string_view b) noexcept {
    const auto lower
        = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; };
    return std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/// finds Content-Length in a header block (the other defined header, Content-Type, is ignored)
std::optional<size_t> content_length(std::string_view headers) {
    while (!headers.empty()) {
        const size_t eol = headers.find(LINE_TERMINATOR);
        const std::string_view line = headers.substr(0, eol);
        headers = (eol == std::string_view::npos) ? std::string_view{}
                                                  : headers.substr(eol + LINE_TERMINATOR.size());
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos
            || !equals_ignoring_case(trim(line.substr(0, colon)), "content-length")) {
            continue;
        }
        const std::string_view value = trim(line.substr(colon + 1));
        size_t len = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), len);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return std::nullopt;
        }
        return len;
    }
    return std::nullopt;
}

} // namespace

std::string_view to_string(TransportError err) noexcept {
    switch (err) {
    case TransportError::end_of_stream:
        return "end of stream";
    case TransportError::io_error:
        return "i/o error";
    case TransportError::malformed_header:
        return "malformed message header";
    }
    std::unreachable();
}

std::expected<std::optional<std::string>, TransportError> Transport::read_message(int timeout_ms) {
    while (true) {
        auto message = take_buffered();
        if (!message || message->has_value()) {
            return message;
        }
        auto filled = fill(timeout_ms);
        if (!filled) {
            return std::unexpected{filled.error()};
        }
        if (!*filled) {
            return std::nullopt; // timed out
        }
        // part of a message has arrived, so the rest is imminent: wait for it unconditionally
        timeout_ms = -1;
    }
}

std::expected<std::optional<std::string>, TransportError> Transport::take_buffered() {
    const std::string_view pending = std::string_view{in_buffer}.substr(in_pos);
    const size_t header_end = pending.find(HEADER_TERMINATOR);
    if (header_end == std::string_view::npos) {
        if (pending.size() > MAX_HEADER_BYTES) {
            return std::unexpected{TransportError::malformed_header};
        }
        return std::nullopt;
    }
    const std::optional<size_t> body_len = content_length(pending.substr(0, header_end));
    if (!body_len) {
        return std::unexpected{TransportError::malformed_header};
    }
    const size_t body_start = header_end + HEADER_TERMINATOR.size();
    if (pending.size() - body_start < *body_len) {
        return std::nullopt; // body not fully here yet
    }
    std::string body{pending.substr(body_start, *body_len)};
    in_pos += body_start + *body_len;
    // compact once the consumed prefix dominates, keeping appends amortized O(1)
    if (in_pos == in_buffer.size()) {
        in_buffer.clear();
        in_pos = 0;
    } else if (in_pos > in_buffer.size() / 2) {
        in_buffer.erase(0, in_pos);
        in_pos = 0;
    }
    return body;
}

std::expected<bool, TransportError> Transport::fill(int timeout_ms) {
    pollfd pfd{.fd = in_fd, .events = POLLIN, .revents = 0};
    while (true) {
        const int ready = ::poll(&pfd, 1, timeout_ms);
        if (ready > 0) {
            break;
        }
        if (ready == 0) {
            return false;
        }
        if (errno != EINTR) {
            return std::unexpected{TransportError::io_error};
        }
    }

    const size_t old_size = in_buffer.size();
    ssize_t got = 0;
    in_buffer.resize_and_overwrite(old_size + READ_CHUNK_BYTES, [&](char* data, size_t) {
        do {
            got = ::read(in_fd, data + old_size, READ_CHUNK_BYTES);
        } while (got < 0 && errno == EINTR);
        return old_size + (got > 0 ? static_cast<size_t>(got) : 0);
    });
    if (got == 0) {
        return std::unexpected{TransportError::end_of_stream};
    }
    if (got < 0) {
        return std::unexpected{TransportError::io_error};
    }
    return true;
}

bool Transport::write_message(std::string_view body) {
    out_buffer.clear();
    out_buffer += "Content-Length: ";
    std::array<char, 24> len_buf{};
    const auto [len_end, ec]
        = std::to_chars(len_buf.data(), len_buf.data() + len_buf.size(), body.size());
    out_buffer.append(len_buf.data(), len_end);
    out_buffer += HEADER_TERMINATOR;
    out_buffer += body;

    std::string_view remaining = out_buffer;
    while (!remaining.empty()) {
        const ssize_t written = ::write(out_fd, remaining.data(), remaining.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        remaining.remove_prefix(static_cast<size_t>(written));
    }
    return true;
}

} // namespace lsp
