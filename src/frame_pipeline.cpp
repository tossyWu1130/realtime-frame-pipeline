#include "frame_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace {

constexpr std::uint32_t kMagic = 0x314D5246u;  // 'FRM1'
constexpr std::uint32_t kWidth = 1024;
constexpr std::uint32_t kHeight = 512;
constexpr std::size_t kSampleStride = 64;

// FNV-1a, four bytes at a time. Byte-at-a-time over 512 KB per frame costs more than the
// frame is worth; the word-wise version is not slower to read and keeps the checksum honest.
std::uint32_t hash32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t hash = 2166136261u;

    std::size_t offset = 0;
    for (; offset + 4 <= size; offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, data + offset, sizeof(word));
        hash = (hash ^ word) * 16777619u;
    }
    for (; offset < size; ++offset) {
        hash = (hash ^ data[offset]) * 16777619u;
    }
    return hash;
}

std::uint64_t nowNs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

FramePipeline::FramePipeline() = default;

FramePipeline::~FramePipeline()
{
    stop();
}

void FramePipeline::start()
{
    if (running_.exchange(true)) {
        return;
    }

    consumer_ = std::thread([this] { consume(); });

    // 10 ms period = 100 Hz.
    timer_.addTask(std::chrono::microseconds(10000), 0, [this] { produce(); });
    timer_.start();
}

void FramePipeline::stop()
{
    if (!running_.exchange(false)) {
        return;
    }

    timer_.stop();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
    }
    queue_cv_.notify_all();
    if (consumer_.joinable()) {
        consumer_.join();
    }
    stopRecording();
}

void FramePipeline::fillPayload(std::vector<std::uint8_t>& payload, std::uint32_t frame_index) const
{
    // 1024 x 512 at one byte per pixel = 512 KB per frame -> ~50 MB/s at 100 Hz.
    const std::size_t expected = static_cast<std::size_t>(kWidth) * kHeight;
    if (payload.size() != expected) {
        payload.resize(expected);
    }

    // A slow triangle envelope, so the statistic on screen actually moves. The number is
    // computed from this data, not simulated.
    const int phase = static_cast<int>(frame_index % 160);
    const int amplitude = (phase < 80 ? 8 + phase * 3 : 8 + (160 - phase) * 3);  // 8 .. 248

    // Written sparsely: only the offsets the consumer samples are refreshed, which keeps the
    // frame cheap to build without faking the measurement.
    for (std::size_t i = 0; i < payload.size(); i += kSampleStride) {
        const int value =
            (((static_cast<int>(i / kSampleStride) + static_cast<int>(frame_index)) & 0xFF) *
             amplitude) >>
            8;
        payload[i] = static_cast<std::uint8_t>(value);
    }
}

void FramePipeline::produce()
{
    const std::uint32_t index = next_index_++;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= kMaxQueue) {
            // The consumer is behind. Refuse the frame instead of growing memory without bound;
            // the index gap makes the loss visible downstream.
            queue_drops_.fetch_add(1);
            return;
        }
    }

    // Reuse a buffer instead of allocating 512 KB per frame. Allocating through the OS at
    // 100 Hz costs page faults that show up directly as scheduling jitter.
    Frame frame;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!free_pool_.empty()) {
            frame = std::move(free_pool_.front());
            free_pool_.pop_front();
        }
    }

    fillPayload(frame.payload, index);

    frame.header.magic = kMagic;
    frame.header.frame_index = index;
    frame.header.timestamp_ns = nowNs();
    frame.header.width = kWidth;
    frame.header.height = kHeight;
    frame.header.payload_bytes = static_cast<std::uint32_t>(frame.payload.size());
    frame.header.payload_hash = hash32(frame.payload.data(), frame.payload.size());

    generated_.fetch_add(1);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.push_back(std::move(frame));
    }
    queue_cv_.notify_one();
}

void FramePipeline::consume()
{
    std::uint32_t last_index = 0;
    bool have_last = false;

    for (;;) {
        Frame frame;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !running_.load() || !queue_.empty(); });

            if (!queue_.empty()) {
                frame = std::move(queue_.front());
                queue_.pop_front();
            } else if (!running_.load()) {
                return;
            } else {
                continue;
            }
        }

        if (frame.header.magic == kMagic) {
            if (have_last && frame.header.frame_index > last_index + 1) {
                lost_.fetch_add(frame.header.frame_index - last_index - 1);
            }
            last_index = frame.header.frame_index;
            have_last = true;

            // Reported statistic is computed on a decimated sample so the demo stays responsive.
            // The pipeline itself is not sampled.
            double sum = 0.0;
            double sum_sq = 0.0;
            std::size_t samples = 0;
            for (std::size_t i = 0; i < frame.payload.size(); i += kSampleStride) {
                const double value = static_cast<double>(frame.payload[i]);
                sum += value;
                sum_sq += value * value;
                ++samples;
            }
            if (samples > 0) {
                const double mean = sum / static_cast<double>(samples);
                const double mean_sq = sum_sq / static_cast<double>(samples);
                const double variance = mean_sq - mean * mean;

                variance_.store(variance);
                std::lock_guard<std::mutex> lock(history_mutex_);
                variance_history_.push_back(variance);
                while (variance_history_.size() > 240) {
                    variance_history_.pop_front();
                }
            }

            const std::uint64_t arrived = nowNs();
            const std::uint64_t age_ns =
                arrived > frame.header.timestamp_ns ? arrived - frame.header.timestamp_ns : 0;
            latency_ms_.store(static_cast<double>(age_ns) / 1.0e6);

            received_.fetch_add(1);
            appendToLog(frame);
        }

        // Hand the buffer back so the producer can refill it. The payload keeps its size on
        // purpose: resizing to zero and back would re-zero 512 KB every frame.
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (free_pool_.size() < kMaxQueue) {
                free_pool_.push_back(std::move(frame));
            }
        }
    }
}

void FramePipeline::appendToLog(const Frame& frame)
{
    bool hit_limit = false;
    {
        std::lock_guard<std::mutex> lock(record_mutex_);

        if (record_requested_ && !recording_) {
            record_stream_.open(record_path_, std::ios::binary | std::ios::trunc);
            recording_ = record_stream_.is_open();
            if (!recording_) {
                record_requested_ = false;
            }
        }
        if (!recording_) {
            return;
        }

        record_stream_.write(reinterpret_cast<const char*>(&frame.header), sizeof(FrameHeader));
        record_stream_.write(reinterpret_cast<const char*>(frame.payload.data()),
                             static_cast<std::streamsize>(frame.payload.size()));
        record_stream_.flush();

        const std::uint64_t count = written_.fetch_add(1) + 1;
        hit_limit = record_limit_ != 0 && count >= record_limit_;
    }

    if (hit_limit) {
        stopRecording();
    }
}

void FramePipeline::startRecording(const std::string& path, std::uint64_t max_frames)
{
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_path_ = path;
    record_limit_ = max_frames;
    written_.store(0);
    record_requested_ = true;
}

void FramePipeline::stopRecording()
{
    std::lock_guard<std::mutex> lock(record_mutex_);
    record_requested_ = false;
    if (recording_) {
        record_stream_.close();
        recording_ = false;
    }
}

FramePipeline::Stats FramePipeline::stats() const
{
    Stats result;
    result.generated = generated_.load();
    result.received = received_.load();
    result.lost = lost_.load();
    result.queue_drops = queue_drops_.load();
    result.written = written_.load();
    result.variance = variance_.load();
    result.latency_ms = latency_ms_.load();
    const PrecisionTimer::Lateness late = timer_.lateness();
    result.jitter_p50_us = late.p50_us;
    result.jitter_p95_us = late.p95_us;
    {
        std::lock_guard<std::mutex> lock(record_mutex_);
        result.recording = recording_;
    }
    return result;
}

std::vector<double> FramePipeline::varianceHistory() const
{
    std::lock_guard<std::mutex> lock(history_mutex_);
    return std::vector<double>(variance_history_.begin(), variance_history_.end());
}
