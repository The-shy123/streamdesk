#include <streamcore/frame_parser.hpp>
#include <algorithm>

namespace streamcore {
FrameChunk FrameParser::push(std::string_view input) {
    if (failed_ || finished_) throw ProtocolError("parser closed; reset required");
    FrameChunk result;
    while (header_size_ < 4 && result.consumed < input.size()) {
        header_[header_size_++] = static_cast<unsigned char>(input[result.consumed++]);
    }
    if (header_size_ != 4) return result;
    if (wanted_ == 0) {
        std::uint32_t length = 0;
        for (auto byte : header_) length = (length << 8) | byte;
        if (length == 0 || length > max_body_bytes) {
            failed_ = true;
            throw ProtocolError("frame length must be 1..65536");
        }
        wanted_ = length;
        body_.reserve(wanted_);
    }
    const auto count = std::min(wanted_ - body_.size(), input.size() - result.consumed);
    if (count) body_.append(input.data() + result.consumed, count);
    result.consumed += count;
    if (body_.size() == wanted_) {
        result.body = std::move(body_);
        body_.clear();
        wanted_ = 0;
        header_size_ = 0;
    }
    return result;
}

void FrameParser::finish() {
    if (failed_) throw ProtocolError("parser failed; reset required");
    if (header_size_ != 0) {
        failed_ = true;
        throw ProtocolError("truncated frame at EOF");
    }
    finished_ = true;
}
void FrameParser::reset() noexcept {
    header_size_ = 0;
    wanted_ = 0;
    body_.clear();
    failed_ = false;
    finished_ = false;
}
std::string encode_frame(std::string_view body) {
    if (body.empty() || body.size() > max_body_bytes) throw ProtocolError("frame body length out of range");
    const auto length = static_cast<std::uint32_t>(body.size());
    std::string frame(4, '\0');
    for (int i = 0; i < 4; ++i) frame[i] = static_cast<char>((length >> (24 - 8 * i)) & 0xff);
    frame.append(body.data(), body.size());
    return frame;
}
}  // namespace streamcore
