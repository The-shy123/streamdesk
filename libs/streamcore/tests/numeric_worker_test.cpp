#include <streamcore/numeric_worker.hpp>
#include <chrono>
#include <iostream>
#include <vector>

namespace {
int checks = 0;
void check(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }
std::unique_ptr<streamcore::NumericMessage> task(std::uint64_t seq = 0) {
    auto result = std::make_unique<streamcore::NumericMessage>();
    result->source_id = "sim"; result->stream_id = "s"; result->session_id = "run";
    result->name = "value"; result->value = 1; result->seq = seq;
    return result;
}
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, open = false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return open; });
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; })) throw std::runtime_error("worker start timeout");
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); open = true; cv.notify_all(); }
};
struct Unblock { Gate& gate; ~Unblock() { gate.release(); } };
void capacity(std::size_t count, std::size_t bytes) {
    Gate gate;
    streamcore::NumericWorker worker(count, bytes, [&](const auto&) { gate.block(); });
    Unblock unblock{gate};
    auto first = task();
    check(worker.try_submit(first) == streamcore::SubmitResult::Accepted && !first, "accepted consumes");
    gate.wait_entered();
    auto next = task(); auto* address = next.get();
    check(worker.try_submit(next) == streamcore::SubmitResult::Busy && next.get() == address, "busy preserves input");
    const auto active = worker.stats();
    check(active.outstanding == 1 && active.peak_count <= count && active.peak_bytes <= bytes, "active counts toward capacity");
    worker.request_stop();
    check(worker.try_submit(next) == streamcore::SubmitResult::Stopped && next.get() == address, "stopped preserves input");
    gate.release();
    worker.stop(); worker.stop();
    const auto done = worker.stats();
    check(done.accepted == 1 && done.processed == 1 && done.outstanding == 0 && done.bytes == 0 && !done.accepting, "drained and released");
}
}
int main() {
    try {
        const auto bytes = streamcore::encode_numeric(*task()).size();
        capacity(1, bytes * 10); // Count limit with available byte capacity.
        capacity(10, bytes);   // Byte limit with available item capacity.
        streamcore::NumericWorker small(10, bytes - 1, [](const auto&) {});
        auto input = task(); auto* ptr = input.get();
        check(small.try_submit(input) == streamcore::SubmitResult::TooLarge && input.get() == ptr, "too large preserves");
        small.stop();
        std::vector<std::uint64_t> seen;
        streamcore::NumericWorker worker(4, 4096, [&](const auto& m) {
            seen.push_back(m.seq);
            if (m.seq == 1) throw std::runtime_error("injected task failure");
        });
        for (std::uint64_t seq = 0; seq < 3; ++seq) {
            auto item = task(seq);
            check(worker.try_submit(item) == streamcore::SubmitResult::Accepted, "accept FIFO task");
        }
        auto invalid = task(); invalid->source_id = "";
        bool rejected = false;
        try { worker.try_submit(invalid); } catch (const streamcore::ProtocolError&) { rejected = true; }
        check(rejected && invalid, "invalid model is distinct from Busy and preserved");
        worker.stop();
        check(seen == std::vector<std::uint64_t>{0,1,2}, "FIFO continues after task exception");
        const auto result = worker.stats();
        check(result.accepted == 3 && result.processed == 2 && result.failed == 1 && result.outstanding == 0, "observable task accounting");
        check(result.last_error == "injected task failure", "observable error");
        streamcore::NumericWorker empty(1, 1024, [](const auto&) {});
        empty.stop(); empty.stop();
        check(empty.stats().accepted == 0, "stop empty worker");
        std::cout << "PASS: worker " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
