#pragma once

// A small reusable scheduler for periodic tasks that need to hold an interval rather than
// "roughly every so often". Tasks are measured against a steady clock and scheduled from the
// *ideal* deadline, so a late wake-up does not push the following ticks back with it.
//
// Usage:
//   PrecisionTimer timer;
//   timer.addTask(std::chrono::milliseconds(100), 0, [] { /* ... */ });  // 0 = run forever
//   timer.start();
//   ...
//   timer.stop();
//
// Add every task before calling start(). Tasks are not meant to be added while running.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class PrecisionTimer
{
public:
    using Interval = std::chrono::microseconds;
    using Clock = std::chrono::steady_clock;

    // How late the ticks actually fired. Reported as a distribution, because a mean alone is
    // misleading: a handful of 12 ms wake-ups disappear inside an average that looks fine.
    struct Lateness {
        std::uint64_t ticks = 0;
        double p50_us = 0.0;
        double p95_us = 0.0;
        double max_us = 0.0;
    };

    PrecisionTimer() = default;

    ~PrecisionTimer()
    {
        stop();
    }

    PrecisionTimer(const PrecisionTimer&) = delete;
    PrecisionTimer& operator=(const PrecisionTimer&) = delete;

    // interval: period between ticks. repeat: how many ticks to fire, 0 means "until stop()".
    void addTask(Interval interval, std::size_t repeat, std::function<void()> fn)
    {
        tasks_.push_back(Task{interval, repeat, 0, std::move(fn), Clock::now() + interval});
    }

    void start()
    {
        if (running_.exchange(true)) {
            return;
        }
        raiseTimerResolution();
        worker_ = std::thread([this] { run(); });
    }

    void stop()
    {
        if (!running_.exchange(false)) {
            return;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        restoreTimerResolution();
    }

    std::uint64_t ticks() const
    {
        return ticks_.load();
    }

    Lateness lateness() const
    {
        std::vector<std::int64_t> copy;
        {
            std::lock_guard<std::mutex> lock(samples_mutex_);
            copy.assign(samples_.begin(), samples_.end());
        }

        Lateness result;
        result.ticks = ticks_.load();
        if (copy.empty()) {
            return result;
        }

        // p50 and p95 are taken from the same window so they describe the same run.
        result.max_us = static_cast<double>(*std::max_element(copy.begin(), copy.end()));

        const std::size_t p95 = copy.size() > 1 ? (copy.size() * 95) / 100 : 0;
        std::nth_element(copy.begin(), copy.begin() + p95, copy.end());
        result.p95_us = static_cast<double>(copy[p95]);

        const std::size_t mid = copy.size() / 2;
        std::nth_element(copy.begin(), copy.begin() + mid, copy.begin() + p95);
        result.p50_us = static_cast<double>(copy[mid]);
        return result;
    }

private:
    // Windows quantises waits to the current timer resolution, which defaults to ~15.6 ms.
    // A 10 ms schedule cannot hold on top of that, so the resolution is raised to 1 ms while
    // the scheduler is running and restored afterwards. No-ops on other platforms.
    static void raiseTimerResolution();
    static void restoreTimerResolution();

    // Raises the calling thread's priority for the duration of the run. On Windows the default
    // priority loses the wake-up race to whatever else the desktop is doing.
    static void configureTimerThread();

    // Blocks until `deadline`. Uses a high-resolution waitable timer on Windows, because sleep
    // and condition-variable waits are quantised to the system tick and timeBeginPeriod cannot
    // be relied on. Returns early (true) if the deadline has already passed.
    static bool waitUntil(Clock::time_point deadline);

    struct Task {
        Interval interval{};
        std::size_t repeat = 0;
        std::size_t fired = 0;
        std::function<void()> fn;
        Clock::time_point next{};
    };

    void record(std::int64_t lateness_us)
    {
        std::lock_guard<std::mutex> lock(samples_mutex_);
        if (samples_.size() >= kSamples) {
            samples_.erase(samples_.begin());
        }
        samples_.push_back(lateness_us);
    }

    void run()
    {
        configureTimerThread();

        while (running_.load(std::memory_order_acquire)) {
            Task* pick = nullptr;
            for (auto& task : tasks_) {
                if (task.repeat != 0 && task.fired >= task.repeat) {
                    continue;
                }
                if (pick == nullptr || task.next < pick->next) {
                    pick = &task;
                }
            }
            if (pick == nullptr) {
                break;  // every task has exhausted its repeat count
            }

            const Clock::time_point now = Clock::now();
            if (pick->next > now) {
                // Sliced at 10 ms so stop() stays responsive even for long periods.
                const Clock::time_point slice = now + std::chrono::milliseconds(10);
                waitUntil(pick->next < slice ? pick->next : slice);
                continue;
            }

            const Clock::time_point scheduled = pick->next;
            const Clock::time_point actual = Clock::now();
            const auto lateness =
                std::chrono::duration_cast<std::chrono::microseconds>(actual - scheduled).count();

            ticks_.fetch_add(1);
            record(static_cast<std::int64_t>(lateness));
            ++pick->fired;
            pick->next += pick->interval;  // advance from the ideal deadline, not from now

            if (pick->fn) {
                pick->fn();
            }
        }
    }

    static constexpr std::size_t kSamples = 240;

    std::vector<Task> tasks_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> ticks_{0};

    mutable std::mutex samples_mutex_;
    std::vector<std::int64_t> samples_;
};
