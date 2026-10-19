#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace streamcore {

inline constexpr std::size_t max_body_bytes = 64 * 1024;

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct SourceTime {
    std::uint64_t value = 0;
    std::string unit = "us";
};

// Every string is owned. No field borrows from the input JSON buffer.
struct NumericMessage {
    int schema_version = 1;
    std::string source_id;
    std::string stream_id;
    std::string session_id;
    std::uint64_t seq = 0;
    std::string kind = "numeric";
    std::optional<SourceTime> source_time;
    std::string name;
    double value = 0;
    std::optional<std::string> unit;
};

// Invalid input throws ProtocolError. Allocation failures are not disguised.
void validate_numeric(const NumericMessage& message);
NumericMessage decode_numeric(std::string_view body);
std::string encode_numeric(const NumericMessage& message);

}  // namespace streamcore
