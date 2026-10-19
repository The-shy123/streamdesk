#pragma once
#include <streamcore/recording.hpp>
#include <vector>

namespace streamcore {
enum class RecordingStatus { Unchecked, Ready, Incomplete, Invalid, Unavailable };
const char* recording_status_name(RecordingStatus status) noexcept;

struct RecordingEntry {
    std::filesystem::path relative_path;
    RecordingStatus status = RecordingStatus::Unchecked;
    std::optional<std::uintmax_t> bytes;
    std::optional<RecordingInfo> info;
    std::string diagnostic;
};
struct CatalogLimits {
    unsigned depth = 1; // Root files plus this many subdirectory levels.
    std::size_t entries = 10000, recordings = 512;
};
struct RecordingCatalog {
    std::filesystem::path root;
    std::vector<RecordingEntry> entries; // Sorted relative paths; IDs are index + 1.
    std::vector<std::string> warnings;
};
struct RecordingDetails {
    RecordingInfo info;
    std::vector<NumericMessage> preview; // At most five records, never a full-file cache.
};

// Read-only discovery. Does not follow child symlinks or validate every file's contents.
// Limit exhaustion throws instead of returning a silently truncated list.
RecordingCatalog scan_recordings(const std::filesystem::path& directory, CatalogLimits limits = {});
// A fresh path/type/content check on every open, not a reuse of cached Ready status.
std::unique_ptr<RecordingReader> open_catalog_recording(RecordingCatalog& catalog, std::size_t id);
RecordingDetails inspect_recording(RecordingCatalog& catalog, std::size_t id);
} // namespace streamcore
