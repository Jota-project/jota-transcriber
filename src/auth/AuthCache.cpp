#include "AuthCache.h"

std::optional<bool> AuthCache::get(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(token);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    if (std::chrono::steady_clock::now() >= it->second.expires_at) {
        entries_.erase(it);
        return std::nullopt;
    }
    return it->second.is_valid;
}

void AuthCache::put(const std::string& token, bool is_valid, int ttl_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[token] = CacheEntry{
        is_valid,
        std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds)
    };
}
