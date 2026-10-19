#include <streamcore/numeric_worker.hpp>
#include <algorithm>
#include <array>

namespace streamcore {
NumericWorker::NumericWorker(std::size_t count, std::size_t bytes, Handler handler)
    : max_count_(count), max_bytes_(bytes), handler_(std::move(handler)) {
    if (count == 0 || bytes == 0 || !handler_) throw std::invalid_argument("positive worker limits and handler required");
    stats_.last_error.reserve(512);
    thread_ = std::thread(&NumericWorker::run, this);
}
NumericWorker::~NumericWorker() { stop(); }
SubmitResult NumericWorker::try_submit(std::unique_ptr<NumericMessage>& input) {
    if (!input) throw std::invalid_argument("null task");
    const auto bytes = encode_numeric(*input).size(); // Validates without consuming.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stats_.accepting) return SubmitResult::Stopped;
    if (bytes > max_bytes_) return SubmitResult::TooLarge;
    if (stats_.outstanding >= max_count_ || bytes > max_bytes_ - stats_.bytes) return SubmitResult::Busy;
    // Allocate the queue slot before moving ownership, preserving input on bad_alloc.
    queue_.emplace_back();
    queue_.back().bytes = bytes;
    queue_.back().message = std::move(input);
    ++stats_.accepted;
    ++stats_.outstanding;
    stats_.bytes += bytes;
    stats_.peak_count = std::max(stats_.peak_count, stats_.outstanding);
    stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.bytes);
    ready_.notify_one();
    return SubmitResult::Accepted;
}
void NumericWorker::request_stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.accepting = false;
    ready_.notify_all();
}
void NumericWorker::stop() {
    request_stop();
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) throw std::logic_error("worker cannot join itself");
        thread_.join();
    }
}
WorkerStats NumericWorker::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}
void NumericWorker::run() {
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [&] { return !queue_.empty() || !stats_.accepting; });
            if (queue_.empty()) return;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        bool failed = false;
        std::array<char, 512> error{};
        std::size_t error_size = 0;
        try { handler_(*item.message); }
        catch (const std::exception& e) {
            failed = true;
            error_size = std::min<std::size_t>(error.size(), std::char_traits<char>::length(e.what()));
            std::copy_n(e.what(), error_size, error.data());
        }
        catch (...) {
            failed = true;
            constexpr std::string_view message = "unknown task exception";
            error_size = message.size();
            std::copy(message.begin(), message.end(), error.begin());
        }
        item.message.reset(); // Release owned input before making its capacity available.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failed) { ++stats_.failed; stats_.last_error.assign(error.data(), error_size); }
            else ++stats_.processed;
            --stats_.outstanding;
            stats_.bytes -= item.bytes;
        }
    }
}
}  // namespace streamcore
