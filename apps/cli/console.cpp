#include "console.hpp"
#include <charconv>
#include <cerrno>
#include <stdexcept>
#include <utility>
#include <poll.h>
#include <unistd.h>

namespace streamdesk_cli {
volatile std::sig_atomic_t interrupted = 0;
namespace { void on_signal(int value) { interrupted = value; } }
void install_signals() {
    interrupted = 0;
    std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
}
unsigned bounded_number(const std::string& text, unsigned maximum) {
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size() || value > maximum)
        throw std::invalid_argument("invalid number or value outside bounds");
    return value;
}
std::string quote(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20 || c == 0x7f) {
            result += "\\u00"; result += digits[c >> 4]; result += digits[c & 15];
        } else result += static_cast<char>(c);
    }
    result += '"'; return result;
}
Input CommandInput::read(int timeout_ms) {
    for (;;) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&input, 1, timeout_ms);
        timeout_ms = 0;
        if (ready < 0) {
            if (errno == EINTR) return {InputKind::None, {}};
            throw std::runtime_error("command input polling failed");
        }
        if (!ready) return {InputKind::None, {}};
        if (input.revents & (POLLERR | POLLNVAL)) throw std::runtime_error("command input unavailable");
        // Read only through one newline, so returning from playback never loses browser commands.
        char byte = 0;
        const auto count = ::read(STDIN_FILENO, &byte, 1);
        if (count < 0) {
            if (errno == EINTR) return {InputKind::None, {}};
            throw std::runtime_error("command input read failed");
        }
        if (count == 0) {
            if (pending_.empty()) return {InputKind::End, {}};
            return {InputKind::Line, std::exchange(pending_, {})};
        }
        if (byte == '\n') {
            if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
            return {InputKind::Line, std::exchange(pending_, {})};
        }
        if (pending_.size() == 128) throw std::runtime_error("command too long (maximum 128 bytes)");
        pending_ += byte;
    }
}
} // namespace streamdesk_cli
