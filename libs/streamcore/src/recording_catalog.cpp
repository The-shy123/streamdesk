#include <streamcore/recording_catalog.hpp>
#include <algorithm>
#include <functional>

namespace streamcore {
namespace fs = std::filesystem;
namespace {
bool candidate(const fs::path& path) {
    return path.extension() == ".sdr" || (path.extension() == ".partial" && path.stem().extension() == ".sdr");
}
void inspect_type(const fs::path& path, RecordingEntry& entry) {
    entry.info.reset(); entry.bytes.reset(); entry.diagnostic.clear();
    entry.status = RecordingStatus::Unavailable;
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || !fs::exists(status)) { entry.diagnostic = "file missing or inaccessible"; return; }
    if (fs::is_symlink(status)) { entry.diagnostic = "symbolic links are not opened"; return; }
    if (!fs::is_regular_file(status)) { entry.diagnostic = "not a regular recording file"; return; }
    const auto bytes = fs::file_size(path, error);
    if (error) { entry.diagnostic = "cannot read file size"; return; }
    entry.bytes = bytes;
    if (path.extension() == ".partial") {
        entry.status = RecordingStatus::Incomplete;
        entry.diagnostic = "unfinished or unpublished recording; replay disabled";
    } else if (bytes > max_recording_bytes) {
        entry.status = RecordingStatus::Invalid;
        entry.diagnostic = "recording file exceeds 16 MiB quota";
    } else entry.status = RecordingStatus::Unchecked;
}
fs::path checked_path(const RecordingCatalog& catalog, const RecordingEntry& entry) {
    if (fs::canonical(catalog.root) != catalog.root || entry.relative_path.empty() || entry.relative_path.is_absolute())
        throw ProtocolError("recording directory changed; refresh the list");
    auto path = catalog.root;
    for (const auto& component : entry.relative_path) {
        if (component == ".." || component == ".") throw ProtocolError("invalid catalog path");
        path /= component;
        if (fs::is_symlink(fs::symlink_status(path))) throw ProtocolError("recording path became a symbolic link");
    }
    return path;
}
}
const char* recording_status_name(RecordingStatus status) noexcept {
    switch (status) {
        case RecordingStatus::Unchecked: return "unchecked";
        case RecordingStatus::Ready: return "ready";
        case RecordingStatus::Incomplete: return "incomplete";
        case RecordingStatus::Invalid: return "invalid";
        case RecordingStatus::Unavailable: return "unavailable";
    }
    return "unavailable";
}
RecordingCatalog scan_recordings(const fs::path& directory, CatalogLimits limits) {
    if (limits.depth > 3 || limits.entries == 0 || limits.entries > 10000 ||
        limits.recordings == 0 || limits.recordings > 512)
        throw std::invalid_argument("catalog limits: depth 0..3, entries 1..10000, recordings 1..512");
    RecordingCatalog result;
    result.root = fs::canonical(directory);
    if (!fs::is_directory(result.root)) throw ProtocolError("catalog path must be a directory");
    std::size_t visited = 0;
    std::function<void(const fs::path&, unsigned)> visit = [&](const fs::path& dir, unsigned depth) {
        std::error_code error;
        fs::directory_iterator iterator(dir, error), end;
        if (error) {
            if (dir == result.root) throw ProtocolError("cannot list recording directory");
            result.warnings.push_back("cannot list directory: " + dir.lexically_relative(result.root).generic_string());
            return;
        }
        while (iterator != end) {
            if (++visited > limits.entries) throw ProtocolError("catalog entry limit exceeded; choose a smaller directory/depth");
            const auto path = iterator->path();
            const auto status = iterator->symlink_status(error);
            if (candidate(path)) {
                if (result.entries.size() == limits.recordings)
                    throw ProtocolError("catalog recording limit exceeded; choose a smaller directory/depth");
                RecordingEntry entry;
                entry.relative_path = path.lexically_relative(result.root);
                inspect_type(path, entry);
                result.entries.push_back(std::move(entry));
            }
            if (error) result.warnings.push_back("cannot inspect entry: " + path.lexically_relative(result.root).generic_string());
            else if (fs::is_directory(status) && depth < limits.depth) visit(path, depth+1);
            iterator.increment(error);
            if (error) {
                result.warnings.push_back("directory enumeration interrupted: " + dir.lexically_relative(result.root).generic_string());
                break;
            }
        }
    };
    visit(result.root, 0);
    std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) {
        return a.relative_path.generic_string() < b.relative_path.generic_string();
    });
    return result;
}
std::unique_ptr<RecordingReader> open_catalog_recording(RecordingCatalog& catalog, std::size_t id) {
    if (id == 0 || id > catalog.entries.size()) throw ProtocolError("record ID outside current list");
    auto& entry = catalog.entries[id-1];
    fs::path path;
    try { path = checked_path(catalog, entry); }
    catch (const fs::filesystem_error& error) {
        entry.status = RecordingStatus::Unavailable; entry.info.reset(); entry.bytes.reset();
        entry.diagnostic = error.what(); throw;
    } catch (const ProtocolError& error) {
        entry.status = RecordingStatus::Unavailable; entry.info.reset(); entry.bytes.reset();
        entry.diagnostic = error.what(); throw;
    }
    inspect_type(path, entry);
    if (entry.status != RecordingStatus::Unchecked) throw ProtocolError(entry.diagnostic);
    try {
        auto reader = std::make_unique<RecordingReader>(path);
        entry.info = reader->info(); entry.status = RecordingStatus::Ready;
        return reader;
    } catch (const ProtocolError& error) {
        entry.status = RecordingStatus::Invalid; entry.diagnostic = error.what(); throw;
    } catch (const fs::filesystem_error& error) {
        entry.status = RecordingStatus::Unavailable; entry.diagnostic = error.what(); throw;
    }
}
RecordingDetails inspect_recording(RecordingCatalog& catalog, std::size_t id) {
    auto reader = open_catalog_recording(catalog, id);
    RecordingDetails result{reader->info(), {}};
    for (std::size_t i = 0; i < std::min<std::size_t>(5, result.info.count); ++i) {
        auto value = reader->next();
        if (!value) throw ProtocolError("recording changed during preview");
        result.preview.push_back(std::move(*value));
    }
    return result;
}
} // namespace streamcore
