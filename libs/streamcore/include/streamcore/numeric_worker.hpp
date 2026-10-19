#pragma once
#include <streamcore/numeric.hpp>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace streamcore {
enum class SubmitResult { Accepted, Busy, Stopped, TooLarge };
struct WorkerStats {
    std::size_t accepted = 0, processed = 0, failed = 0;
    std::size_t outstanding = 0, bytes = 0, peak_count = 0, peak_bytes = 0;
    bool accepting = true;
    std::string last_error;
};

// One owner controls join/stop/destruction; no concurrent destruction or self-join.
// Limits include the active task as well as queued tasks. Byte accounting uses
// validated canonical JSON size, computed internally, not a caller's estimate.
class NumericWorker {
public:
    using Handler = std::function<void(const NumericMessage&)>;
    NumericWorker(std::size_t max_count, std::size_t max_bytes, Handler handler);
    ~NumericWorker();
    NumericWorker(const NumericWorker&) = delete;
    NumericWorker& operator=(const NumericWorker&) = delete;
    SubmitResult try_submit(std::unique_ptr<NumericMessage>& input);
    void request_stop(); // Non-blocking Running -> Draining.
    void stop();         // Owner only: drain accepted tasks and join; repeatable.
    WorkerStats stats() const;
private:
    struct Item { std::unique_ptr<NumericMessage> message; std::size_t bytes = 0; };
    void run();
    const std::size_t max_count_, max_bytes_;
    Handler handler_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Item> queue_;
    WorkerStats stats_;
    std::thread thread_;
};
}  // namespace streamcore
