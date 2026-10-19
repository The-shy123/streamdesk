#include <streamcore/numeric.hpp>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("usage: streamdesk_inspect verification.jsonl (not a recording format)");
        std::ifstream file(argv[1]);
        if (!file) throw std::runtime_error("cannot open input");
        std::array<char, streamcore::max_body_bytes + 2> line{};
        std::size_t count = 0;
        double sum = 0;
        std::optional<streamcore::NumericMessage> previous;
        while (file.getline(line.data(), static_cast<std::streamsize>(line.size()))) {
            if (count == 10000) throw streamcore::ProtocolError("inspection record limit exceeded");
            auto bytes = static_cast<std::size_t>(file.gcount());
            if (!file.eof()) --bytes; // getline counts the extracted newline, which is not stored.
            auto message = streamcore::decode_numeric(std::string_view(line.data(), bytes));
            if (previous && (message.source_id != previous->source_id || message.stream_id != previous->stream_id ||
                message.session_id != previous->session_id || previous->seq == std::numeric_limits<std::uint64_t>::max() ||
                message.seq != previous->seq + 1)) throw streamcore::ProtocolError("stream/session/sequence discontinuity");
            const double next_sum = sum + message.value;
            if (!std::isfinite(next_sum)) throw streamcore::ProtocolError("aggregate overflow");
            if (count < 5) std::cout << "SAMPLE " << streamcore::encode_numeric(message) << '\n';
            sum = next_sum; ++count; previous = std::move(message);
        }
        if (!file.eof()) throw streamcore::ProtocolError("input line too long or read failed");
        if (count == 0) throw streamcore::ProtocolError("empty verification file");
        std::cout << "INSPECT {\"count\":" << count << ",\"sum\":" << std::setprecision(20) << sum << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n'; return 1;
    }
}
