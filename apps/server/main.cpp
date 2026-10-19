#include <streamcore/frame_parser.hpp>
#include <streamcore/numeric_worker.hpp>
#include <streamcore/recording.hpp>
#include <boost/asio.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>

namespace asio = boost::asio;
using Tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;

struct Options {
    unsigned port = 0, deadline_seconds = 30, queue_count = 32, queue_bytes = 262144;
    unsigned max_records = 10000;
    std::string output, record;
    unsigned record_count = 0;
};
unsigned number(const std::string& text, unsigned max) {
    unsigned result = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result > max)
        throw std::invalid_argument("invalid numeric option");
    return result;
}
Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) throw std::invalid_argument("option needs a value");
        const std::string key = argv[i], value = argv[i+1];
        if (key == "--port") result.port = number(value, 65535);
        else if (key == "--deadline-seconds") result.deadline_seconds = number(value, 300);
        else if (key == "--queue-count") result.queue_count = number(value, 1024);
        else if (key == "--queue-bytes") result.queue_bytes = number(value, 64 * 1024 * 1024);
        else if (key == "--max-records") result.max_records = number(value, 10000);
        else if (key == "--output") result.output = value;
        else if (key == "--record") result.record = value;
        else if (key == "--record-count") result.record_count = number(value, 10000);
        else throw std::invalid_argument("unknown option");
    }
    if (!result.deadline_seconds || !result.queue_count || !result.queue_bytes || !result.max_records)
        throw std::invalid_argument("limits must be positive");
    if (result.record.empty() != (result.record_count == 0) || result.record_count > result.max_records)
        throw std::invalid_argument("--record requires --record-count 1..max-records (at most 10000)");
    if (!result.record.empty() && !result.output.empty()) {
        const auto output = std::filesystem::absolute(result.output).lexically_normal();
        if (output == std::filesystem::absolute(result.record).lexically_normal() ||
            output == std::filesystem::absolute(result.record + ".partial").lexically_normal())
            throw std::invalid_argument("verification output and recording paths must differ");
    }
    return result;
}

class Server {
public:
    explicit Server(Options opts)
        : options_(std::move(opts)), acceptor_(io_, Tcp::endpoint(asio::ip::address_v4::loopback(), options_.port)),
          socket_(io_), signals_(io_, SIGINT, SIGTERM), deadline_(io_), idle_(io_), retry_(io_),
          worker_(options_.queue_count, options_.queue_bytes, [this](const auto& message) { process(message); }) {
        if (!options_.output.empty()) {
            if (std::filesystem::exists(options_.output)) throw std::invalid_argument("output already exists; choose a new path");
            output_.open(options_.output);
            if (!output_) throw std::runtime_error("cannot open output");
        }
        if (!options_.record.empty())
            recording_ = std::make_unique<streamcore::RecordingWriter>(options_.record, options_.record_count);
    }
    int run() {
        deadline_.expires_after(std::chrono::seconds(options_.deadline_seconds));
        deadline_.async_wait([this](auto error) { if (!error) close("failed", "overall deadline exceeded"); });
        signals_.async_wait([this](auto error, int) { if (!error) close("stopped", "stop signal received"); });
        acceptor_.async_accept(socket_, [this](auto error) {
            if (closing_) return;
            if (error) { close("failed", "accept failed"); return; }
            acceptor_.close(); // Exactly one source connection in this demo.
            std::cout << "CONNECTED\n" << std::flush;
            arm_idle();
            read();
        });
        std::cout << "READY 127.0.0.1 " << acceptor_.local_endpoint().port() << '\n' << std::flush;
        io_.run();
        // Never join inside the network event loop.
        worker_.stop();
        const auto stats = worker_.stats();
        if (stats.failed) { state_ = "failed"; reason_ = "worker failure: " + stats.last_error; }
        if (recording_ && state_ == "completed") {
            try { recording_->finish(); }
            catch (const std::exception& error) { state_ = "failed"; reason_ = error.what(); }
        }
        if (recording_) {
            const bool completed = state_ == "completed";
            std::cout << "RECORDING " << (completed ? "completed" : state_ == "stopped" ? "interrupted" : "failed")
                      << ' ' << options_.record << (completed ? "" : ".partial") << '\n';
        }
        if (!reason_.empty()) std::cerr << reason_ << '\n';
        if (latest_) std::cout << "LATEST " << streamcore::encode_numeric(*latest_) << '\n';
        std::cout << "SUMMARY {\"status\":\"" << state_ << "\",\"accepted\":" << stats.accepted
                  << ",\"processed\":" << stats.processed << ",\"failed\":" << stats.failed
                  << ",\"invalid\":" << invalid_ << ",\"discarded_pending\":" << discarded_
                  << ",\"busy\":" << busy_count_ << ",\"peak_count\":" << stats.peak_count
                  << ",\"peak_bytes\":" << stats.peak_bytes << ",\"sum\":" << std::setprecision(20) << sum_ << "}\n";
        return state_ == "failed" ? 1 : 0;
    }
private:
    void process(const streamcore::NumericMessage& message) {
        // Runs only on the worker thread. All reads of these fields happen after join.
        const double next_sum = sum_ + message.value;
        if (!std::isfinite(next_sum)) throw std::runtime_error("aggregate overflow");
        const auto body = streamcore::encode_numeric(message);
        if (output_.is_open()) {
            if (written_bytes_ + body.size() + 1 > 16 * 1024 * 1024) throw std::runtime_error("verification output limit exceeded");
            output_ << body << '\n'; output_.flush();
            if (!output_) throw std::runtime_error("verification output write failed");
            written_bytes_ += body.size() + 1;
        }
        if (recording_) recording_->append(message);
        // Keep terminal preview bounded; complete verification uses the optional file.
        if (preview_count_++ < 20) std::cout << "VALUE " << message.seq << ' ' << message.name << ' ' << message.value << '\n' << std::flush;
        sum_ = next_sum;
        latest_ = message;
    }
    void arm_idle() {
        idle_.expires_after(std::chrono::seconds(3));
        idle_.async_wait([this](auto error) { if (!error) close("failed", "input idle/partial-message timeout"); });
    }
    void read() {
        if (closing_) return;
        socket_.async_read_some(asio::buffer(buffer_), [this](auto error, std::size_t size) {
            if (closing_) return;
            if (error && error != asio::error::eof) { close("failed", "socket read error"); return; }
            eof_ = error == asio::error::eof;
            position_ = 0; size_ = size;
            if (size) arm_idle();
            pump();
        });
    }
    void check_session(const streamcore::NumericMessage& message) {
        if (previous_) {
            if (message.source_id != previous_->source_id || message.stream_id != previous_->stream_id ||
                message.session_id != previous_->session_id) throw streamcore::ProtocolError("source/stream/session changed");
            if (previous_->seq == std::numeric_limits<std::uint64_t>::max() || message.seq != previous_->seq + 1)
                throw streamcore::ProtocolError("duplicate or missing sequence");
        }
    }
    void pump() {
        if (closing_) return;
        try {
            for (;;) {
                if (pending_) {
                    const auto result = worker_.try_submit(pending_);
                    if (result == streamcore::SubmitResult::Busy) {
                        ++busy_count_;
                        if (!busy_since_) busy_since_ = Clock::now();
                        if (Clock::now() - *busy_since_ > std::chrono::seconds(1)) {
                            close("failed", "queue overload timeout"); return;
                        }
                        // Retain one pending model and one 4 KiB read buffer; pause reads.
                        retry_.expires_after(std::chrono::milliseconds(2));
                        retry_.async_wait([this](auto error) { if (!error) pump(); });
                        return;
                    }
                    if (result == streamcore::SubmitResult::TooLarge) { close("failed", "task exceeds queue byte capacity"); return; }
                    if (result == streamcore::SubmitResult::Stopped) { close("stopped", "worker already stopping"); return; }
                    busy_since_.reset();
                }
                if (position_ < size_) {
                    auto result = parser_.push(std::string_view(buffer_.data() + position_, size_ - position_));
                    position_ += result.consumed;
                    if (result.body) {
                        if (received_ >= options_.max_records ||
                            (recording_ && received_ >= options_.record_count))
                            throw streamcore::ProtocolError("record limit exceeded");
                        auto message = streamcore::decode_numeric(*result.body);
                        check_session(message);
                        previous_ = message;
                        ++received_;
                        pending_ = std::make_unique<streamcore::NumericMessage>(std::move(message));
                    }
                    continue;
                }
                if (eof_) { parser_.finish(); close("completed", ""); }
                else read();
                return;
            }
        } catch (const streamcore::ProtocolError& error) {
            ++invalid_; close("failed", error.what());
        } catch (const std::exception& error) { close("failed", error.what()); }
    }
    void close(const char* state, std::string reason) {
        if (closing_) return;
        closing_ = true; state_ = state; reason_ = std::move(reason);
        if (pending_) { ++discarded_; pending_.reset(); }
        boost::system::error_code ignored;
        socket_.cancel(ignored); socket_.close(ignored); acceptor_.close(ignored);
        deadline_.cancel(); idle_.cancel(); retry_.cancel(); signals_.cancel();
        worker_.request_stop();
    }
    Options options_;
    asio::io_context io_;
    Tcp::acceptor acceptor_;
    Tcp::socket socket_;
    asio::signal_set signals_;
    asio::steady_timer deadline_, idle_, retry_;
    std::array<char, 4096> buffer_{};
    std::size_t position_ = 0, size_ = 0, received_ = 0, invalid_ = 0, discarded_ = 0, busy_count_ = 0;
    bool closing_ = false, eof_ = false;
    std::string state_ = "failed", reason_;
    streamcore::FrameParser parser_;
    std::unique_ptr<streamcore::NumericMessage> pending_;
    std::optional<streamcore::NumericMessage> previous_;
    std::optional<Clock::time_point> busy_since_;
    std::ofstream output_;
    std::size_t written_bytes_ = 0, preview_count_ = 0;
    double sum_ = 0;
    std::optional<streamcore::NumericMessage> latest_;
    std::unique_ptr<streamcore::RecordingWriter> recording_;
    streamcore::NumericWorker worker_;
};
int main(int argc, char** argv) {
    try { Server server(options(argc, argv)); return server.run(); }
    catch (const std::exception& error) { std::cerr << "ERROR: " << error.what() << '\n'; return 1; }
}
