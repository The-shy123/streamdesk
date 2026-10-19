#include <streamcore/recording_catalog.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <sys/stat.h>

namespace fs = std::filesystem;
using namespace streamcore;
namespace {
int checks = 0;
void check(bool ok, const std::string& reason) { ++checks; if (!ok) throw std::runtime_error(reason); }
void rejects(const std::function<void()>& action, const std::string& reason) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, reason);
}
struct Workspace {
    fs::path path;
    Workspace() {
        for (int i = 0; i < 100; ++i) {
            path = fs::temp_directory_path() / ("streamdesk-catalog-test-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(i));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create test workspace");
    }
    ~Workspace() { std::error_code ignored; fs::remove_all(path, ignored); }
};
void save(const fs::path& path, const std::string& session = "first") {
    RecordingWriter writer(path, 6);
    for (unsigned i = 0; i < 6; ++i) {
        NumericMessage value;
        value.source_id = "sim"; value.stream_id = "values"; value.session_id = session;
        value.seq = i+10; value.name = "温度"; value.value = i*0.5;
        writer.append(value);
    }
    writer.finish();
}
std::size_t id(const RecordingCatalog& catalog, const fs::path& relative) {
    for (std::size_t i = 0; i < catalog.entries.size(); ++i)
        if (catalog.entries[i].relative_path == relative) return i+1;
    throw std::runtime_error("fixture missing from catalog: "+relative.string());
}
}
int main() {
    try {
        Workspace work;
        auto root = work.path/"records"; fs::create_directory(root);
        check(scan_recordings(root).entries.empty(), "empty directory supported");
        save(root/"a 空格.sdr");
        std::ofstream(root/"b.sdr.partial") << "unfinished";
        std::ofstream(root/"c.sdr") << "not a recording";
        std::ofstream(root/"ignore.jsonl") << "ignored";
        fs::create_directories(root/"sub/deeper");
        save(root/"sub/child.sdr"); save(root/"sub/deeper/deep.sdr");
        fs::create_directory(work.path/"outside"); save(work.path/"outside/external.sdr");
        fs::create_directory_symlink(work.path/"outside", root/"linked");
        fs::create_symlink(work.path/"outside/external.sdr", root/"linked-file.sdr");
        check(::mkfifo((root/"pipe.sdr").c_str(), 0600) == 0, "special-file fixture");
        auto catalog = scan_recordings(root);
        check(catalog.entries.size() == 6, "default depth, suffix filtering, no symlink-directory traversal");
        for (std::size_t i = 1; i < catalog.entries.size(); ++i)
            check(catalog.entries[i-1].relative_path.generic_string() < catalog.entries[i].relative_path.generic_string(), "stable sorted paths");
        const auto good = id(catalog, "a 空格.sdr"), partial = id(catalog,"b.sdr.partial"), bad = id(catalog,"c.sdr");
        check(catalog.entries[good-1].status == RecordingStatus::Unchecked, "discovery does not pretend to validate");
        check(catalog.entries[bad-1].status == RecordingStatus::Unchecked, "bad content not parsed during discovery");
        check(catalog.entries[partial-1].status == RecordingStatus::Incomplete, "partial classified without parsing");
        check(catalog.entries[id(catalog,"pipe.sdr")-1].status == RecordingStatus::Unavailable, "FIFO not opened");
        check(catalog.entries[id(catalog,"linked-file.sdr")-1].status == RecordingStatus::Unavailable, "symlink not opened");
        auto detail = inspect_recording(catalog, good);
        check(detail.info.count == 6 && detail.info.first_seq == 10 && detail.info.last_seq == 15 &&
              detail.info.session_id == "first", "verified metadata");
        check(detail.preview.size() == 5 && detail.preview.front().seq == 10 && detail.preview.back().seq == 14,
              "preview capped at five, ordered");
        check(catalog.entries[good-1].status == RecordingStatus::Ready, "verified status updated");
        auto reader = open_catalog_recording(catalog, good);
        check(reader->next()->seq == 10, "preview does not advance the next replay session");
        reader.reset();
        rejects([&] { inspect_recording(catalog, bad); }, "bad file rejected on selection");
        check(catalog.entries[bad-1].status == RecordingStatus::Invalid && !catalog.entries[bad-1].info,
              "invalid content has no trusted metadata");
        rejects([&] { open_catalog_recording(catalog, partial); }, "partial cannot replay");
        rejects([&] { open_catalog_recording(catalog, 0); }, "zero ID");
        rejects([&] { open_catalog_recording(catalog, catalog.entries.size()+1); }, "out-of-range ID");
        check(scan_recordings(root, {0,10000,512}).entries.size() == 5, "depth zero");
        check(scan_recordings(root, {2,10000,512}).entries.size() == 7, "depth two");
        rejects([&] { scan_recordings(root, {1,1,512}); }, "enumeration quota cannot silently truncate");
        rejects([&] { scan_recordings(root, {1,10000,1}); }, "record count quota cannot silently truncate");
        rejects([&] { scan_recordings(root, {4,10000,512}); }, "depth cap");
        rejects([&] { scan_recordings(root/"missing"); }, "missing directory");
        rejects([&] { scan_recordings(root/"a 空格.sdr"); }, "file is not a catalog root");
        save(root/"0-new.sdr", "new");
        check(id(catalog,"a 空格.sdr") == good, "new files do not shift current snapshot IDs");
        auto refreshed = scan_recordings(root);
        check(id(refreshed,"a 空格.sdr") == good+1, "explicit refresh discovers and renumbers");
        std::ofstream(root/"a 空格.sdr", std::ios::trunc) << "damaged after inspection";
        rejects([&] { open_catalog_recording(catalog, good); }, "Ready status never bypasses a fresh validation");
        check(catalog.entries[good-1].status == RecordingStatus::Invalid, "cached metadata invalidated");
        fs::remove(root/"a 空格.sdr");
        rejects([&] { open_catalog_recording(catalog, good); }, "deleted selection handled");
        check(catalog.entries[good-1].status == RecordingStatus::Unavailable, "deleted row status");
        fs::create_symlink(work.path/"outside/external.sdr", root/"a 空格.sdr");
        rejects([&] { open_catalog_recording(catalog, good); }, "replaced file symlink rejected");
        const auto child = id(catalog,"sub/child.sdr");
        fs::rename(root/"sub", root/"original-sub");
        fs::create_directory_symlink(root/"original-sub", root/"sub");
        rejects([&] { open_catalog_recording(catalog, child); }, "parent replaced by symlink rejected");
        auto huge = root/"huge.sdr"; std::ofstream(huge).close(); fs::resize_file(huge,max_recording_bytes+1);
        auto limited = scan_recordings(root);
        check(limited.entries[id(limited,"huge.sdr")-1].status == RecordingStatus::Invalid, "oversized file classified without opening");
        std::cout << "PASS: recording catalog " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
