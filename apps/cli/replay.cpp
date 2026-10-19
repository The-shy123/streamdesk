#include "replay.hpp"
#include <chrono>
#include <cmath>
#include <cerrno>
#include <iomanip>
#include <iostream>
#include <poll.h>

namespace streamdesk_cli {
int replay(streamcore::RecordingReader& reader, bool interactive, unsigned delay, CommandInput& input) {
    const auto total = reader.info().count;
    std::size_t played = 0;
    double sum = 0;
    bool playing = !interactive, stopped = false, completed = false;
    using Clock = std::chrono::steady_clock;
    auto due = Clock::now();
    auto state = [&] {
        std::cout << "STATE " << (completed ? "completed" : stopped ? "stopped" : playing ? "playing" : "paused")
                  << ' ' << played << '/' << total << '\n' << std::flush;
    };
    auto emit = [&] {
        auto message = reader.next();
        if (!message) throw streamcore::ProtocolError("unexpected replay end");
        const double next_sum = sum + message->value;
        if (!std::isfinite(next_sum)) throw streamcore::ProtocolError("replay aggregate overflow");
        if (played+1 == total && reader.next()) throw streamcore::ProtocolError("unexpected extra record");
        std::cout << "VALUE " << streamcore::encode_numeric(*message) << '\n' << std::flush;
        sum = next_sum; ++played;
        completed = played == total;
        due = Clock::now() + std::chrono::milliseconds(delay);
    };
    auto command = [&](const std::string& text) {
        if (text == "play" || text == "resume") { playing = true; due = Clock::now(); }
        else if (text == "pause") playing = false;
        else if (text == "next") { playing = false; emit(); }
        else if (text == "stop" || text == "quit") stopped = true;
        else if (text != "status") {
            std::cout << "COMMAND_ERROR use play, pause, resume, next, status, stop\n" << std::flush;
            return;
        }
        state();
    };
    std::cout << "REPLAY_READY " << total << '\n' << std::flush;
    state();
    while (!stopped && !completed) {
        if (interrupted) { stopped = true; break; }
        // One control loop owns both reader and playback state; no prefetch queue.
        int wait = playing && Clock::now() >= due ? 0 : 20;
        if (interactive) {
            auto event = input.read(wait);
            if (event.kind == InputKind::End) { stopped = true; break; }
            if (event.kind == InputKind::Line) command(event.text);
        } else if (::poll(nullptr, 0, wait) < 0 && errno != EINTR) {
            throw std::runtime_error("replay wait failed");
        }
        if (interrupted) { stopped = true; break; }
        if (playing && !stopped && !completed && Clock::now() >= due) emit();
    }
    std::cout << "REPLAY {\"status\":\"" << (completed ? "completed" : "stopped")
              << "\",\"count\":" << played << ",\"total\":" << total << ",\"sum\":"
              << std::setprecision(20) << sum << "}\n";
    return 0;
}
} // namespace streamdesk_cli
