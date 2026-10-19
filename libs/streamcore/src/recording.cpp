#include <streamcore/recording.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>

namespace streamcore {
namespace {
using Json = nlohmann::json;
constexpr std::uint64_t hash_seed = 14695981039346656037ULL;
void require(bool ok, const char* reason) { if (!ok) throw ProtocolError(reason); }
void hash_line(std::uint64_t& hash, std::string_view line) {
    for (unsigned char byte : line) { hash ^= byte; hash *= 1099511628211ULL; }
    hash ^= '\n'; hash *= 1099511628211ULL;
}
std::string hex(std::uint64_t hash) {
    std::ostringstream text;
    text << std::hex << std::setw(16) << std::setfill('0') << hash;
    return text.str();
}
Json header(const NumericMessage& message, std::size_t count) {
    return {{"format", "streamdesk.numeric"}, {"version", 1}, {"expected_count", count},
            {"source_id", message.source_id}, {"stream_id", message.stream_id},
            {"session_id", message.session_id}, {"first_seq", std::to_string(message.seq)}};
}
Json footer(std::size_t count, std::uint64_t last, std::uint64_t hash) {
    return {{"recording_end", "completed"}, {"count", count}, {"last_seq", std::to_string(last)},
            {"checksum_fnv1a64", hex(hash)}};
}
void continuity(const NumericMessage& previous, const NumericMessage& message) {
    require(previous.source_id == message.source_id && previous.stream_id == message.stream_id &&
            previous.session_id == message.session_id, "recording stream/session changed");
    require(previous.seq != std::numeric_limits<std::uint64_t>::max() && message.seq == previous.seq + 1,
            "recording sequence discontinuity");
}
Json parse(const std::string& line) {
    require(line.find('\0') == std::string::npos, "raw NUL in recording");
    try {
        auto object = Json::parse(line, [](int depth, Json::parse_event_t, Json&) {
            require(depth <= 8, "recording JSON nesting exceeds limit"); return true;
        });
        // Our container has canonical compact JSON lines: rejects duplicate keys too.
        require(object.is_object() && object.dump() == line, "noncanonical recording JSON");
        return object;
    } catch (const Json::exception&) { throw ProtocolError("invalid recording JSON"); }
}
} // namespace

struct RecordingWriter::Impl {
    std::filesystem::path path, partial;
    std::FILE* file = nullptr;
    std::size_t expected, limit, bytes = 0, count = 0;
    std::uint64_t hash = hash_seed;
    std::optional<NumericMessage> previous;
    bool failed = false, completed = false;
    std::exception_ptr failure;
    Impl(std::filesystem::path destination, std::size_t expected_count, std::size_t byte_limit)
        : path(std::move(destination)), partial(path.string() + ".partial"),
          expected(expected_count), limit(byte_limit) {
        require(expected > 0 && expected <= max_recording_records, "record count must be 1..10000");
        require(limit > 0 && limit <= max_recording_bytes, "recording byte limit outside bounds");
        require(!path.empty() && path.string().find('\0') == std::string::npos, "invalid recording path");
        require(path.extension() != ".partial", ".partial is reserved for unfinished recordings");
        require(!std::filesystem::exists(std::filesystem::symlink_status(path)), "recording already exists");
        // POSIX file creation is isolated here; tested on macOS, no other-platform claim.
        int fd = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd == -1) throw ProtocolError("cannot reserve recording .partial (exists or path unavailable)");
        file = ::fdopen(fd, "wb");
        if (!file) { ::close(fd); throw ProtocolError("cannot open recording stream"); }
    }
    ~Impl() { if (file) std::fclose(file); }
    void write(const std::string& line, bool checksum = true) {
        require(line.size() + 1 <= limit - bytes, "recording byte quota exceeded");
        require(std::fwrite(line.data(), 1, line.size(), file) == line.size() &&
                std::fputc('\n', file) != EOF && std::fflush(file) == 0, "recording write failed");
        bytes += line.size() + 1;
        if (checksum) hash_line(hash, line);
    }
};
RecordingWriter::RecordingWriter(std::filesystem::path path, std::size_t count, std::size_t limit)
    : impl_(std::make_unique<Impl>(std::move(path), count, limit)) {}
RecordingWriter::~RecordingWriter() = default;
void RecordingWriter::append(const NumericMessage& message) {
    auto& state = *impl_;
    if (state.failure) std::rethrow_exception(state.failure);
    require(!state.failed && !state.completed, "recording writer is closed or failed");
    try {
        require(state.count < state.expected, "recording expected count exceeded");
        const auto body = encode_numeric(message);
        if (state.previous) continuity(*state.previous, message);
        else state.write(header(message, state.expected).dump());
        state.write(body);
        state.previous = message;
        ++state.count;
    } catch (...) { state.failed = true; state.failure = std::current_exception(); throw; }
}
void RecordingWriter::finish() {
    auto& state = *impl_;
    if (state.completed) return;
    if (state.failure) std::rethrow_exception(state.failure);
    require(!state.failed, "recording writer failed");
    try {
        require(state.count == state.expected, "recording incomplete: expected count not reached");
        state.write(footer(state.count, state.previous->seq, state.hash).dump(), false);
        auto* file = state.file; state.file = nullptr;
        require(std::fclose(file) == 0, "recording close failed");
        // A hard link publishes only a fully closed file and cannot replace an existing path.
        std::filesystem::create_hard_link(state.partial, state.path);
        state.completed = true;
        std::error_code ignored;
        std::filesystem::remove(state.partial, ignored); // A leftover alias is safe, never a replacement.
    } catch (...) { state.failed = true; state.failure = std::current_exception(); throw; }
}

struct RecordingReader::Impl {
    std::ifstream file;
    Json metadata;
    RecordingInfo info, verified_info;
    std::size_t expected = 0, bytes = 0, count = 0;
    std::uint64_t hash = hash_seed;
    std::optional<NumericMessage> previous;
    bool ended = false, failed = false;
    explicit Impl(const std::filesystem::path& path) {
        require(path.string().find('\0') == std::string::npos, "invalid recording path");
        require(path.extension() != ".partial", "recording interrupted/unpublished: .partial file");
        require(std::filesystem::is_regular_file(path), "recording must be a regular file");
        require(std::filesystem::file_size(path) <= max_recording_bytes, "recording file exceeds quota");
        file.open(path, std::ios::binary);
        require(file.is_open(), "cannot open recording");
        begin();
        while (next()) {}
        verified_info = info;
        file.clear(); file.seekg(0);
        require(static_cast<bool>(file), "cannot rewind recording");
        begin();
    }
    std::optional<std::string> line() {
        std::array<char, max_body_bytes + 2> buffer{};
        file.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto extracted = static_cast<std::size_t>(file.gcount());
        if (extracted == 0 && file.eof() && !file.bad()) return std::nullopt;
        require(!file.fail() && !file.eof() && extracted > 0, "recording truncated or line too long");
        require(extracted - 1 <= max_body_bytes && extracted <= max_recording_bytes - bytes,
                "recording size limit exceeded");
        bytes += extracted;
        return std::string(buffer.data(), extracted - 1);
    }
    void begin() {
        bytes = count = 0; hash = hash_seed; previous.reset(); ended = failed = false;
        auto first = line(); require(first.has_value(), "empty recording");
        metadata = parse(*first);
        require(metadata.contains("format") && metadata["format"] == "streamdesk.numeric" &&
                metadata.contains("version") && metadata["version"].is_number_unsigned() &&
                metadata["version"] == 1, "unknown recording format/version");
        require(metadata.contains("expected_count") && metadata["expected_count"].is_number_unsigned(),
                "missing recording expected count");
        const auto n = metadata["expected_count"].get<std::uint64_t>();
        require(n > 0 && n <= max_recording_records, "invalid recording expected count");
        expected = static_cast<std::size_t>(n);
        hash_line(hash, *first);
    }
    std::optional<NumericMessage> next() {
        require(!failed, "recording reader failed");
        if (ended) return std::nullopt;
        try {
            auto body = line(); require(body.has_value(), "recording interrupted: missing completion footer");
            (void)parse(*body);
            if (count == expected) {
                require(*body == footer(count, previous->seq, hash).dump(), "recording completion/checksum mismatch");
                require(!line().has_value(), "trailing data after recording completion");
                ended = true; return std::nullopt;
            }
            auto message = decode_numeric(*body);
            if (previous) continuity(*previous, message);
            else {
                require(metadata == header(message, expected), "recording header/stream mismatch");
                info.source_id = message.source_id; info.stream_id = message.stream_id;
                info.session_id = message.session_id; info.first_seq = message.seq;
                info.count = expected;
            }
            hash_line(hash, *body);
            info.last_seq = message.seq; previous = message; ++count;
            return message;
        } catch (...) { failed = true; throw; }
    }
};
RecordingReader::RecordingReader(const std::filesystem::path& path) : impl_(std::make_unique<Impl>(path)) {}
RecordingReader::~RecordingReader() = default;
const RecordingInfo& RecordingReader::info() const noexcept { return impl_->verified_info; }
std::optional<NumericMessage> RecordingReader::next() { return impl_->next(); }
} // namespace streamcore
