#include "ConnectionLimiter.h"

ConnectionLimiter::ConnectionLimiter(size_t max_total, size_t max_per_ip)
    : max_total_(max_total), max_per_ip_(max_per_ip), total_(0) {}

bool ConnectionLimiter::tryAcquire(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_ >= max_total_) {
        return false;
    }

    size_t& count = per_ip_[ip];
    if (count >= max_per_ip_) {
        return false;
    }

    ++count;
    ++total_;
    return true;
}

void ConnectionLimiter::release(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = per_ip_.find(ip);
    if (it != per_ip_.end() && it->second > 0) {
        --it->second;
        if (it->second == 0) {
            per_ip_.erase(it);
        }
    }
    if (total_ > 0) {
        --total_;
    }
}

std::string ConnectionLimiter::getMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return "transcription_active_connections " + std::to_string(total_) + "\n" +
           "transcription_max_connections " + std::to_string(max_total_) + "\n";
}
