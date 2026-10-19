#include <streamcore/recording.hpp>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;
using namespace streamcore;
namespace {
int checks = 0;
void check(bool ok, const std::string& label) {
    ++checks; if (!ok) throw std::runtime_error(label);
}
void rejects(const std::function<void()>& action, const std::string& label) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, label);
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary); file << text;
    if (!file) throw std::runtime_error("fixture write failed");
}
std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at == std::string::npos) throw std::runtime_error("fixture replacement missing");
    text.replace(at, from.size(), to); return text;
}
NumericMessage sample(std::uint64_t seq) {
    NumericMessage value;
    value.source_id = "sim"; value.stream_id = "values"; value.session_id = "run";
    value.seq = seq; value.name = "温度"; value.value = 1.5; value.unit = "°C";
    value.source_time = SourceTime{seq, "us"}; return value;
}
struct Workspace {
    fs::path path;
    Workspace() {
        for (int n = 0; n < 100; ++n) {
            path = fs::temp_directory_path() / ("streamdesk-record-test-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(n));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create isolated test directory");
    }
    ~Workspace() { std::error_code ignored; fs::remove_all(path, ignored); }
};
}
int main() {
    try {
        Workspace work;
        auto path = work.path / "good.sdr";
        {
            RecordingWriter writer(path, 3);
            check(!fs::exists(path) && fs::exists(path.string()+".partial"), "exclusive partial before completion");
            rejects([&] { RecordingWriter other(path, 3); }, "concurrent destination reservation rejected");
            for (int i = 0; i < 3; ++i) writer.append(sample(9007199254740993ULL+i));
            writer.finish(); writer.finish();
            rejects([&] { writer.append(sample(0)); }, "append after finish rejected");
        }
        check(fs::exists(path) && !fs::exists(path.string()+".partial"), "published after close");
        RecordingReader reader(path);
        check(reader.info().count == 3 && reader.info().first_seq == 9007199254740993ULL &&
              reader.info().last_seq == 9007199254740995ULL, "verified metadata and exact uint64");
        for (int i = 0; i < 3; ++i) {
            auto value = reader.next();
            check(value && encode_numeric(*value) == encode_numeric(sample(9007199254740993ULL+i)),
                  "roundtrip every field including Unicode/optional time");
            check(reader.info().last_seq == 9007199254740995ULL, "metadata stable during streaming");
        }
        check(!reader.next() && !reader.next(), "repeated EOF");
        const auto valid = read(path);
        rejects([&] { RecordingWriter writer(path, 1); }, "no overwrite existing file");
        check(read(path) == valid, "existing content preserved");
        const auto bad = work.path / "bad.sdr";
        for (const auto& text : {
            std::string(), encode_numeric(sample(0))+"\n", valid.substr(0, valid.size()-1),
            valid.substr(0, valid.rfind('{')), valid+"junk\n",
            replace(valid, "\"version\":1", "\"version\":2"),
            replace(valid, "\"expected_count\":3", "\"expected_count\":4"),
            replace(valid, "\"value\":1.5", "\"value\":2.5"),
            replace(valid, "\"source_id\":\"sim\"", "\"source_id\":\"changed\""),
            replace(valid, "\"count\":3", "\"count\":2"),
            replace(valid, "\"count\":3", "\"count\":3.0"),
            replace(valid, "\"version\":1", "\"version\":1,\"version\":1"),
            valid.substr(0,valid.find('\n')) + std::string("\0junk\n",6),
            std::string(max_body_bytes+1,'x')+"\n"}) {
            write(bad, text);
            rejects([&] { RecordingReader input(bad); }, "damaged/incomplete/unknown file rejected before replay");
        }
        write(bad, ""); fs::resize_file(bad, max_recording_bytes+1);
        rejects([&] { RecordingReader input(bad); }, "file byte quota");
        rejects([&] { RecordingReader input(work.path); }, "non-file input rejected");
        auto partial = work.path / "abandoned.sdr";
        { RecordingWriter writer(partial, 2); writer.append(sample(0)); }
        check(!fs::exists(partial), "destructor never completes");
        rejects([&] { RecordingReader input(partial.string()+".partial"); }, "interrupted suffix rejected");
        fs::copy_file(partial.string()+".partial", bad, fs::copy_options::overwrite_existing);
        rejects([&] { RecordingReader input(bad); }, "renaming incomplete data cannot complete it");
        {
            RecordingWriter writer(work.path/"short.sdr", 2); writer.append(sample(0));
            rejects([&] { writer.finish(); }, "short recording cannot finish");
            rejects([&] { writer.append(sample(1)); }, "failure is sticky");
        }
        {
            RecordingWriter writer(work.path/"quota.sdr", 1, 1);
            rejects([&] { writer.append(sample(0)); }, "bounded write quota failure");
            rejects([&] { writer.finish(); }, "quota failure cannot publish");
        }
        {
            RecordingWriter writer(work.path/"gap.sdr", 2); writer.append(sample(0));
            rejects([&] { writer.append(sample(2)); }, "sequence gap rejected");
        }
        {
            RecordingWriter writer(work.path/"session.sdr", 2); writer.append(sample(0));
            auto changed = sample(1); changed.session_id = "other";
            rejects([&] { writer.append(changed); }, "session change rejected");
        }
        {
            RecordingWriter writer(work.path/"overflow.sdr", 2); writer.append(sample(UINT64_MAX));
            rejects([&] { writer.append(sample(0)); }, "sequence wrap rejected");
        }
        {
            auto destination = work.path/"collision.sdr";
            RecordingWriter writer(destination, 1); writer.append(sample(0));
            write(destination,"preserve");
            rejects([&] { writer.finish(); }, "publication cannot replace a late destination");
            check(read(destination)=="preserve", "late destination preserved");
        }
        {
            auto destination = work.path/"symlink.sdr";
            fs::create_symlink(work.path/"nonexistent", destination.string()+".partial");
            rejects([&] { RecordingWriter writer(destination, 1); }, "partial symlink rejected");
        }
        rejects([&] { RecordingWriter writer(work.path/"zero.sdr", 0); }, "zero count rejected");
        rejects([&] { RecordingWriter writer(work.path/"large.sdr", 10001); }, "large count rejected");
        rejects([&] { RecordingWriter writer(work.path/"reserved.partial", 1); }, "unfinished suffix reserved");
        std::cout << "PASS: recording " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
