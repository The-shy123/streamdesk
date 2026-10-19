#pragma once
#include <streamcore/numeric.hpp>
#include <filesystem>
#include <memory>

namespace streamcore {
inline constexpr std::size_t max_recording_records = 10000;
inline constexpr std::size_t max_recording_bytes = 16 * 1024 * 1024;

struct RecordingInfo {
    std::string source_id, stream_id, session_id;
    std::uint64_t first_seq = 0, last_seq = 0;
    std::size_t count = 0;
};

// One caller at a time; the server appends on its worker, then finishes after join.
// Reserves PATH.partial exclusively. Destruction never publishes an incomplete file.
class RecordingWriter {
public:
    RecordingWriter(std::filesystem::path path, std::size_t expected_count,
                    std::size_t byte_limit = max_recording_bytes);
    ~RecordingWriter();
    RecordingWriter(const RecordingWriter&) = delete;
    RecordingWriter& operator=(const RecordingWriter&) = delete;
    void append(const NumericMessage& message);
    void finish(); // Exact expected count, close successfully, publish without replacement.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Validates the entire bounded file before returning, then streams one record per next().
// No background read-ahead. Do not modify the file during the reader's lifetime.
class RecordingReader {
public:
    explicit RecordingReader(const std::filesystem::path& path);
    ~RecordingReader();
    RecordingReader(const RecordingReader&) = delete;
    RecordingReader& operator=(const RecordingReader&) = delete;
    const RecordingInfo& info() const noexcept;
    std::optional<NumericMessage> next();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace streamcore
