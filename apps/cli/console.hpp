#pragma once
#include <csignal>
#include <string>
#include <string_view>

namespace streamdesk_cli {
extern volatile std::sig_atomic_t interrupted;
void install_signals();
unsigned bounded_number(const std::string& text, unsigned maximum);
std::string quote(std::string_view text); // Escape control bytes in terminal-visible paths/errors.
enum class InputKind { None, Line, End };
struct Input { InputKind kind; std::string text; };
class CommandInput {
public:
    Input read(int timeout_ms);
private:
    std::string pending_;
};
} // namespace streamdesk_cli
