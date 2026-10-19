#include "replay.hpp"
#include <streamcore/recording_catalog.hpp>
#include <iostream>
#include <sstream>

namespace {
using namespace streamcore;
using namespace streamdesk_cli;
void row(const RecordingEntry& entry, std::size_t id) {
    std::cout << "ENTRY " << id << ' ' << recording_status_name(entry.status) << ' '
              << (entry.bytes ? std::to_string(*entry.bytes) : "-") << ' '
              << quote(entry.relative_path.generic_string()) << '\n';
    if (!entry.diagnostic.empty()) std::cout << "NOTE " << id << ' ' << quote(entry.diagnostic) << '\n';
}
void listing(const RecordingCatalog& catalog) {
    std::cout << "DIRECTORY " << quote(catalog.root.string()) << '\n'
              << "ID / status / bytes / relative path\n";
    for (std::size_t i = 0; i < catalog.entries.size(); ++i) row(catalog.entries[i], i+1);
    for (const auto& warning : catalog.warnings) std::cout << "WARNING " << quote(warning) << '\n';
    std::cout << "CATALOG " << catalog.entries.size() << " records\n" << std::flush;
}
void details(RecordingCatalog& catalog, unsigned id) {
    try {
        const auto result = inspect_recording(catalog, id);
        row(catalog.entries.at(id-1), id);
        std::cout << "SOURCE " << quote(result.info.source_id) << '\n'
                  << "STREAM " << quote(result.info.stream_id) << '\n'
                  << "SESSION " << quote(result.info.session_id) << '\n'
                  << "COUNT " << result.info.count << '\n'
                  << "SEQUENCE " << result.info.first_seq << ' ' << result.info.last_seq << '\n';
        for (const auto& value : result.preview) std::cout << "PREVIEW " << encode_numeric(value) << '\n';
        std::cout << "DETAIL " << id << " ready\n" << std::flush;
    } catch (...) {
        if (id > 0 && id <= catalog.entries.size()) row(catalog.entries[id-1], id);
        throw;
    }
}
void help() {
    std::cout << "Commands: list | show ID | replay ID | refresh | help | quit\n"
              << "Replay starts paused: next / play / pause / resume / status / stop.\n"
              << "After replay stops or completes, you return here. Refresh replaces IDs.\n" << std::flush;
}
int run(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "usage: streamdesk_records DIRECTORY [--interactive | --show ID | --replay ID]"
                     " [--depth 0..3] [--interval-ms 0..1000]\n";
        help(); return 0;
    }
    if (argc < 2) throw std::invalid_argument("usage: streamdesk_records DIRECTORY [--interactive | --show ID | --replay ID]");
    CatalogLimits limits;
    bool interactive = false;
    unsigned selected = 0, delay = 100;
    std::string action;
    for (int i = 2; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--interactive") {
            if (!action.empty()) throw std::invalid_argument("select only one browser mode");
            interactive = true; action = "interactive";
        } else if ((key == "--show" || key == "--replay") && i+1 < argc) {
            if (!action.empty()) throw std::invalid_argument("select only one browser mode");
            action = key; selected = bounded_number(argv[++i], 512);
            if (!selected) throw std::invalid_argument("record IDs start at 1");
        } else if (key == "--depth" && i+1 < argc) limits.depth = bounded_number(argv[++i], 3);
        else if (key == "--interval-ms" && i+1 < argc) delay = bounded_number(argv[++i], 1000);
        else throw std::invalid_argument("unknown or incomplete browser option");
    }
    install_signals();
    auto catalog = scan_recordings(argv[1], limits);
    CommandInput input;
    if (action == "--show") { details(catalog, selected); return 0; }
    if (action == "--replay") {
        auto reader = open_catalog_recording(catalog, selected);
        return replay(*reader, false, delay, input);
    }
    listing(catalog);
    if (!interactive) return 0;
    help();
    std::cout << "BROWSER_READY\n" << std::flush;
    while (!interrupted) {
        const auto event = input.read(50);
        if (interrupted || event.kind == InputKind::End) break;
        if (event.kind != InputKind::Line) continue;
        try {
            std::istringstream words(event.text);
            std::string verb, argument, extra;
            words >> verb >> argument >> extra;
            if (verb == "show" || verb == "replay") {
                if (argument.empty() || !extra.empty()) throw std::invalid_argument("use show ID or replay ID");
                const auto id = bounded_number(argument, 512);
                if (verb == "show") details(catalog, id);
                else {
                    auto reader = open_catalog_recording(catalog, id);
                    std::cout << "SELECTED " << id << ' ' << quote(catalog.entries.at(id-1).relative_path.generic_string()) << '\n';
                    replay(*reader, true, delay, input);
                    std::cout << "RETURNED_TO_RECORDS\n";
                }
            } else {
                if (!argument.empty()) throw std::invalid_argument("unexpected command argument");
                if (verb == "quit" || verb == "exit") break;
                if (verb == "list") listing(catalog);
                else if (verb == "refresh") {
                    auto refreshed = scan_recordings(catalog.root, limits);
                    catalog = std::move(refreshed); // Keep old IDs if refresh fails.
                    listing(catalog);
                } else if (verb == "help") help();
                else if (!verb.empty()) throw std::invalid_argument("unknown command; enter help");
            }
        } catch (const ProtocolError& error) {
            std::cout << "ERROR " << quote(error.what()) << '\n';
        } catch (const std::filesystem::filesystem_error& error) {
            std::cout << "ERROR " << quote(error.what()) << '\n';
        } catch (const std::invalid_argument& error) {
            std::cout << "ERROR " << quote(error.what()) << '\n';
        }
        if (!interrupted) std::cout << "BROWSER_READY\n" << std::flush;
    }
    std::cout << "BROWSER_CLOSED\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << "ERROR: " << streamdesk_cli::quote(error.what()) << '\n'; return 1;
    }
}
