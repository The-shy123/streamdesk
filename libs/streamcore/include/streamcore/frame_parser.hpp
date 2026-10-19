#pragma once
#include <streamcore/numeric.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace streamcore {
struct FrameChunk {
    std::size_t consumed = 0;
    std::optional<std::string> body;
};

// One output at most per push. Caller retains and resubmits the unconsumed tail.
class FrameParser {
public:
    FrameChunk push(std::string_view input);
    void finish(); // EOF: rejects an incomplete frame; closes until reset.
    void reset() noexcept;
    std::size_t buffered_bytes() const noexcept { return header_size_ + body_.size(); }
private:
    std::array<unsigned char, 4> header_{};
    std::size_t header_size_ = 0;
    std::size_t wanted_ = 0;
    std::string body_;
    bool failed_ = false;
    bool finished_ = false;
};
std::string encode_frame(std::string_view body);
}  // namespace streamcore
