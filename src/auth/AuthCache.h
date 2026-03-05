#pragma once
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class AuthCache {
public:
    std::optional<bool> get(const std::string& token);
    void put(const std::string& token, bool is_valid, int ttl_seconds);

private:
    struct CacheEntry {
        bool is_valid;
        std::chrono::steady_clock::time_point expires_at;
    };

    std::mutex mutex_;
    std::unordered_map<std::string, CacheEntry> entries_;
};
