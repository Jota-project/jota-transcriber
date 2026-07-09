#pragma once
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <iostream>

/**
 * @brief Singleton for limiting concurrent whisper inference calls.
 *
 * Prevents GPU OOM and massive latency latency spikes by capping
 * the maximum number of simultaneous whisper_full_with_state() executions.
 */
class InferenceLimiter {
public:
    static InferenceLimiter& instance() {
        static InferenceLimiter inst;
        return inst;
    }

    /**
     * @brief Define the maximum number of concurrent inferences.
     */
    void setMaxConcurrency(int max_concurrent) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (max_concurrent > 0) {
            max_concurrent_ = max_concurrent;
            // Unblock waiters in case we increased the limit
            cv_.notify_all();
        }
    }

    // Test-only: reset active count to 0 without affecting max_concurrent_.
    void resetForTesting() {
        std::lock_guard<std::mutex> lock(mutex_);
        active_count_ = 0;
        cv_.notify_all();
    }

    /**
     * @brief Block until an inference slot is available, then claim it.
     */
    void acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return active_count_ < max_concurrent_; });
        ++active_count_;
    }

    /**
     * @brief Try to acquire an inference slot without blocking.
     * @return true if a slot was acquired, false if all slots are taken.
     */
    bool try_acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_count_ < max_concurrent_) {
            ++active_count_;
            return true;
        }
        return false;
    }

    /**
     * @brief Try to acquire an inference slot, waiting up to `timeout` if none is free.
     * @return true if a slot was acquired before the timeout elapsed, false otherwise.
     */
    bool try_acquire_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, timeout, [this]() { return active_count_ < max_concurrent_; })) {
            ++active_count_;
            return true;
        }
        return false;
    }

    /**
     * @brief Release an inference slot, waking up one waiting thread.
     */
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_count_ > 0) {
            --active_count_;
            cv_.notify_one();
        }
    }

    /**
     * @brief Get telemetry metrics in Prometheus format
     */
    std::string getMetrics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return "transcription_active_inferences " + std::to_string(active_count_) + "\n" +
               "transcription_max_inferences " + std::to_string(max_concurrent_) + "\n";
    }

    /**
     * @brief Check if there is capacity for new inferences
     */
    bool hasCapacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_count_ < max_concurrent_;
    }

    // RAII guard for exception-safe acquire/release
    class Guard {
    public:
        Guard() { InferenceLimiter::instance().acquire(); }
        ~Guard() { InferenceLimiter::instance().release(); }
        // Non-copyable/movable
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    // Non-blocking RAII guard. Acquires a slot via try_acquire() on construction.
    // Check acquired() before using. Releases only if a slot was acquired.
    // Non-movable: use std::optional<TryGuard> with emplace() for deferred construction.
    class TryGuard {
    public:
        TryGuard() : acquired_(InferenceLimiter::instance().try_acquire()) {}
        explicit TryGuard(std::chrono::milliseconds timeout)
            : acquired_(InferenceLimiter::instance().try_acquire_for(timeout)) {}
        ~TryGuard() { if (acquired_) InferenceLimiter::instance().release(); }
        bool acquired() const { return acquired_; }
        TryGuard(const TryGuard&) = delete;
        TryGuard& operator=(const TryGuard&) = delete;
        TryGuard(TryGuard&&) = delete;
        TryGuard& operator=(TryGuard&&) = delete;
    private:
        bool acquired_;
    };

private:
    InferenceLimiter() = default;
    ~InferenceLimiter() = default;

    // Non-copyable
    InferenceLimiter(const InferenceLimiter&) = delete;
    InferenceLimiter& operator=(const InferenceLimiter&) = delete;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    int active_count_ = 0;
    int max_concurrent_ = 4; // Default safe limit for a 8GB GPU
};
