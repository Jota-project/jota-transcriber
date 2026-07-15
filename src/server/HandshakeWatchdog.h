#pragma once
#include "server/SocketUtil.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// jota-transcriber#68 covered a session that had already completed its
// WebSocket handshake and then went idle forever. That fix (the flushLoop
// watchdog in StreamingSession) doesn't cover an earlier, wider gap: the
// ConnectionGuard slot in handleSession() is acquired *before* the TLS
// handshake and the initial HTTP upgrade request are read, and neither has
// any timeout at all — a client that opens a TCP connection and sends
// nothing (or trickles data arbitrarily slowly) holds that slot forever.
//
// HandshakeWatchdog closes that gap with the same primitive: a short-lived,
// per-connection background thread that force-shuts the socket down if
// disarm() isn't called before the deadline. Safe to disarm() multiple
// times, including after it has already fired.
class HandshakeWatchdog {
public:
    template <class NativeHandle>
    HandshakeWatchdog(NativeHandle fd, int timeout_sec)
        : armed_(true) {
        thread_ = std::thread([this, fd, timeout_sec]() {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait_for(lk, std::chrono::seconds(timeout_sec), [this] { return !armed_; });
            if (armed_) {
                armed_ = false;
                forceSocketShutdown(fd);
            }
        });
    }

    ~HandshakeWatchdog() {
        disarm();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    HandshakeWatchdog(const HandshakeWatchdog&) = delete;
    HandshakeWatchdog& operator=(const HandshakeWatchdog&) = delete;

    void disarm() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            armed_ = false;
        }
        cv_.notify_one();
    }

private:
    bool armed_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
};
