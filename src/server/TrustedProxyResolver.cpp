#include "TrustedProxyResolver.h"

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sstream>
#include <cctype>

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
}  // namespace

TrustedProxyResolver::TrustedProxyResolver(const std::string& comma_separated_hosts,
                                           int refresh_sec,
                                           ResolveFn resolver)
    : refresh_sec_(refresh_sec),
      resolver_(resolver ? std::move(resolver) : &TrustedProxyResolver::defaultResolver) {
    std::stringstream ss(comma_separated_hosts);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string h = trim(item);
        if (!h.empty()) hosts_.push_back(h);
    }
}

bool TrustedProxyResolver::enabled() const {
    return !hosts_.empty();
}

bool TrustedProxyResolver::isTrusted(const std::string& ip) {
    if (hosts_.empty()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    bool stale = !ever_resolved_ ||
                 (now - last_resolve_ >= std::chrono::seconds(refresh_sec_));
    if (stale) {
        std::unordered_set<std::string> fresh;
        for (const auto& host : hosts_) {
            for (auto& resolved : resolver_(host)) {
                fresh.insert(resolved);
            }
        }
        if (!fresh.empty()) {
            trusted_ips_ = std::move(fresh);
            ever_resolved_ = true;
        }
        // else: keep last known-good set (fail-closed if never resolved).
        last_resolve_ = now;  // bound retry cost to once per refresh window
    }
    return trusted_ips_.count(ip) > 0;
}

// static
std::vector<std::string> TrustedProxyResolver::defaultResolver(const std::string& host) {
    std::vector<std::string> out;
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // IPv4 + IPv6
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) {
        return out;  // empty on failure
    }
    for (auto* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        void* addr = nullptr;
        if (p->ai_family == AF_INET) {
            addr = &reinterpret_cast<struct sockaddr_in*>(p->ai_addr)->sin_addr;
        } else if (p->ai_family == AF_INET6) {
            addr = &reinterpret_cast<struct sockaddr_in6*>(p->ai_addr)->sin6_addr;
        }
        if (addr && inet_ntop(p->ai_family, addr, buf, sizeof(buf))) {
            out.emplace_back(buf);
        }
    }
    freeaddrinfo(res);
    return out;
}
