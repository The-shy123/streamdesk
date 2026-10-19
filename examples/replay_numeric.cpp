#include "replay.hpp"
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::invalid_argument("usage: streamdesk_replay FILE [--interactive] [--interval-ms 0..1000]");
        bool interactive = false;
        unsigned delay = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--interactive") interactive = true;
            else if (key == "--interval-ms" && i+1 < argc)
                delay = streamdesk_cli::bounded_number(argv[++i], 1000);
            else throw std::invalid_argument("unknown or incomplete replay option");
        }
        streamdesk_cli::install_signals();
        streamcore::RecordingReader reader(argv[1]);
        streamdesk_cli::CommandInput input;
        return streamdesk_cli::replay(reader, interactive, delay, input);
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << streamdesk_cli::quote(error.what()) << '\n'; return 1;
    }
}
