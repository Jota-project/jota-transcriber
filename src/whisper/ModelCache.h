#pragma once
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <iostream>
#include <whisper.h>

/**
 * @brief Singleton cache for the whisper model context.
 *
 * Manages the lifecycle of a shared whisper_context using reference counting
 * and a configurable TTL. The model loads on the first acquire() call and
 * unloads automatically after the TTL expires with no active sessions.
 *
 * Thread-safe: all public methods are protected by a mutex.
 *
 * Sessions create their own whisper_state via whisper_init_state() for
 * thread-safe concurrent inference on the shared (read-only) model weights.
 */
class ModelCache {
public:
    static ModelCache& instance() {
        static ModelCache inst;
        return inst;
    }

    /**
     * @brief Configure the cache before first use.
     * @param ttl_seconds  Seconds to keep the model after the last release().
     *                     0 = unload immediately. -1 = keep forever.
     */
    void configure(int ttl_seconds) {
        std::lock_guard<std::mutex> lock(mutex_);
        ttl_seconds_ = ttl_seconds;
    }

    /**
     * @brief Acquire a reference to the model.
     *
     * Loads the model from disk if it is not already cached.
     * Blocks until the model is ready.
     *
     * @param model_path  Path to the .bin model file.
     * @param use_gpu     Whether to use GPU acceleration.
     * @return A valid whisper_context pointer (never null).
     * @throws std::runtime_error if loading fails.
     */
    whisper_context* acquire(const std::string& model_path, bool use_gpu = true) {
        std::lock_guard<std::mutex> lock(mutex_);

        // Cancel any pending unload
        cancelUnloadLocked();

        if (ctx_ && loaded_path_ == model_path) {
            // Model already loaded — just bump the ref count
            ++ref_count_;
            return ctx_;
        }

        // Different model requested while one is loaded — unload first
        if (ctx_) {
            std::cout << "[ModelCache] Unloading previous model: " << loaded_path_ << std::endl;
            whisper_free(ctx_);
            ctx_ = nullptr;
            loaded_path_.clear();
        }

        // Load the model
        std::cout << "[ModelCache] Loading model: " << model_path << std::endl;
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu    = use_gpu;
        cparams.flash_attn = true;

        ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
        if (!ctx_) {
            throw std::runtime_error("[ModelCache] Failed to load whisper model: " + model_path);
        }

        loaded_path_ = model_path;
        ref_count_ = 1;
        std::cout << "[ModelCache] Model loaded successfully" << std::endl;
        return ctx_;
    }

    /**
     * @brief Release a reference to the model.
     *
     * When the ref count reaches zero, a background timer starts. If no new
     * acquire() happens within ttl_seconds_, the model is freed.
     */
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ref_count_ <= 0) return;

        --ref_count_;
        if (ref_count_ == 0) {
            if (ttl_seconds_ == 0) {
                // Immediate unload
                unloadLocked();
            } else if (ttl_seconds_ > 0) {
                // Schedule deferred unload
                scheduleUnloadLocked();
            }
            // ttl < 0 → keep forever (no-op)
        }
    }

    /**
     * @brief Force-unload the model, ignoring ref count.
     */
    void forceUnload() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelUnloadLocked();
        unloadLocked();
    }

    /// Current number of active references.
    int refCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ref_count_;
    }

    /// Whether a model is currently loaded.
    bool isLoaded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ctx_ != nullptr;
    }

    // Non-copyable
    ModelCache(const ModelCache&) = delete;
    ModelCache& operator=(const ModelCache&) = delete;

    ~ModelCache() {
        cancelUnloadLocked();
        if (ctx_) {
            whisper_free(ctx_);
            ctx_ = nullptr;
        }
    }

private:
    ModelCache() = default;

    void unloadLocked() {
        if (ctx_) {
            std::cout << "[ModelCache] Unloading model: " << loaded_path_ << std::endl;
            whisper_free(ctx_);
            ctx_ = nullptr;
            loaded_path_.clear();
            ref_count_ = 0;
        }
    }

    void cancelUnloadLocked() {
        if (unload_pending_.exchange(false)) {
            // The timer thread will check unload_pending_ and skip the unload
        }
    }

    void scheduleUnloadLocked() {
        unload_pending_ = true;
        int ttl = ttl_seconds_;

        // Detach a lightweight timer thread
        std::thread([this, ttl]() {
            std::this_thread::sleep_for(std::chrono::seconds(ttl));

            std::lock_guard<std::mutex> lock(mutex_);
            if (unload_pending_ && ref_count_ == 0) {
                unloadLocked();
                unload_pending_ = false;
            }
        }).detach();
    }

    mutable std::mutex mutex_;
    whisper_context* ctx_ = nullptr;
    std::string loaded_path_;
    int ref_count_ = 0;
    int ttl_seconds_ = 300; // default 5 minutes
    std::atomic<bool> unload_pending_{false};
};
