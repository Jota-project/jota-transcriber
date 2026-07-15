#pragma once
#include <mutex>
#include <unordered_map>
#include <string>

class ConnectionLimiter {
public:
    ConnectionLimiter(size_t max_total, size_t max_per_ip);

    bool tryAcquire(const std::string& ip, bool trusted = false);
    void release(const std::string& ip);

    std::string getMetrics() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, size_t> per_ip_;
    size_t max_total_;
    size_t max_per_ip_;
    size_t total_;
};
