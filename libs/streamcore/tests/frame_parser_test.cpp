#include <streamcore/frame_parser.hpp>
#include <algorithm>
#include <functional>
#include <iostream>
#include <random>
#include <vector>

namespace {
int checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void rejects(const std::function<void()>& f) {
    bool failed = false;
    try { f(); } catch (const streamcore::ProtocolError&) { failed = true; }
    check(failed, "expected protocol failure");
}
std::vector<std::string> parse(const std::string& wire, std::size_t chunk, bool random) {
    streamcore::FrameParser parser;
    std::vector<std::string> messages;
    std::mt19937 rng(20261015);
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const auto size = std::min(wire.size() - offset, random ? std::size_t(1 + rng() % chunk) : chunk);
        std::size_t used = 0;
        while (used < size) {
            auto result = parser.push(std::string_view(wire).substr(offset + used, size - used));
            check(result.consumed > 0, "progress required");
            used += result.consumed;
            if (result.body) messages.push_back(std::move(*result.body));
            check(parser.buffered_bytes() <= streamcore::max_body_bytes + 4, "bounded parser");
        }
        offset += size;
    }
    parser.finish();
    parser.finish(); // Repeated EOF is harmless.
    rejects([&] { parser.push("x"); });
    return messages;
}
}
int main() {
    try {
        const std::vector<std::string> expected{"a", "hello", std::string(257, 'q')};
        std::string wire;
        for (const auto& body : expected) wire += streamcore::encode_frame(body);
        check(static_cast<unsigned char>(wire[3]) == 1, "network byte order");
        for (std::size_t chunk = 1; chunk <= wire.size(); ++chunk) check(parse(wire, chunk, false) == expected, "every split size");
        check(parse(wire, 37, true) == expected, "fixed random chunks");
        const std::string maximum(streamcore::max_body_bytes, 'x');
        check(parse(streamcore::encode_frame(maximum), 8192, false) == std::vector<std::string>{maximum}, "max frame");
        streamcore::FrameParser parser;
        check(parser.push({}).consumed == 0, "empty input");
        for (const std::string header : {std::string(4, '\0'), std::string(4, char(0xff)), std::string("\0\1\0\1", 4)}) {
            parser.reset();
            rejects([&] { parser.push(header); });
            check(parser.buffered_bytes() == 4, "oversize rejected before body allocation");
            rejects([&] { parser.push(wire); });
            rejects([&] { parser.finish(); });
        }
        const auto frame = streamcore::encode_frame("hello");
        for (std::size_t cut = 1; cut < frame.size(); ++cut) {
            parser.reset();
            parser.push(std::string_view(frame).substr(0, cut));
            rejects([&] { parser.finish(); });
        }
        parser.reset();
        auto first = parser.push(wire);
        check(first.body == "a" && first.consumed == 5, "one output with unconsumed tail");
        rejects([] { streamcore::encode_frame(""); });
        rejects([&] { streamcore::encode_frame(maximum + "x"); });
        parser.reset();
        parser.finish(); // Empty stream has no truncated frame.
        std::cout << "PASS: frame parser " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
