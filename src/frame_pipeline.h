#pragma once

#include "precision_timer.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Every frame carries its own identity. The header is deliberately a fixed 512 bytes so that a
// recorded file can be walked frame by frame by any tool, without a side-channel index and
// without knowing anything about the producer's memory layout.
#pragma pack(push, 1)
struct FrameHeader {
    std::uint32_t magic;         // 'FRM1'
    std::uint32_t frame_index;   // monotonically increasing; a jump means frames were lost
    std::uint64_t timestamp_ns;  // producer clock, for end-to-end latency
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t payload_bytes;
    std::uint32_t payload_hash;  // FNV-1a over the payload
    std::uint8_t reserved[480];  // keeps the header at 512 bytes
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 512, "frame header must stay 512 bytes");

struct Frame {
    FrameHeader header{};
    std::vector<std::uint8_t> payload;
};

class FramePipeline
{
public:
    struct Stats {
        std::uint64_t generated = 0;
        std::uint64_t received = 0;
        std::uint64_t lost = 0;         // index discontinuities seen by the consumer
        std::uint64_t queue_drops = 0;  // frames refused by a full queue
        std::uint64_t written = 0;      // frames appended to the .dat log
        double variance = 0.0;          // statistic of the most recent frame
        double latency_ms = 0.0;        // age of the frame just processed
        double jitter_p50_us = 0.0;     // typical tick lateness
        double jitter_p95_us = 0.0;     // tail tick lateness over the same window
        bool recording = false;
    };

    FramePipeline();
    ~FramePipeline();

    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;

    void start();
    void stop();

    void startRecording(const std::string& path, std::uint64_t max_frames);
    void stopRecording();

    Stats stats() const;
    std::vector<double> varianceHistory() const;

private:
    void produce();
    void consume();
    void appendToLog(const Frame& frame);

    void fillPayload(std::vector<std::uint8_t>& payload, std::uint32_t frame_index) const;

    PrecisionTimer timer_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Frame> queue_;
    std::deque<Frame> free_pool_;  // recycled frames: allocating 512 KB per frame starves the pipeline
    static constexpr std::size_t kMaxQueue = 8;

    std::thread consumer_;
    std::atomic<bool> running_{false};

    std::uint32_t next_index_ = 0;

    std::atomic<std::uint64_t> generated_{0};
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> lost_{0};
    std::atomic<std::uint64_t> queue_drops_{0};
    std::atomic<std::uint64_t> written_{0};
    std::atomic<double> variance_{0.0};
    std::atomic<double> latency_ms_{0.0};

    mutable std::mutex record_mutex_;
    bool record_requested_ = false;
    bool recording_ = false;
    std::string record_path_;
    std::uint64_t record_limit_ = 0;
    std::ofstream record_stream_;

    mutable std::mutex history_mutex_;
    std::deque<double> variance_history_;
};
