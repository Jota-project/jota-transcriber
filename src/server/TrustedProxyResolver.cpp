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
                                           ResolveFn resolver)
    : resolver_(resolver ? std::move(resolver) : &TrustedProxyResolver::defaultResolver) {
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

void TrustedProxyResolver::refresh() {
    if (hosts_.empty()) return;

    // Resolve every host WITHOUT holding mutex_ — getaddrinfo can block for
    // a long time during a DNS outage, and isTrusted() must never wait on
    // it. Collect results locally first.
    std::unordered_map<std::string, std::unordered_set<std::string>> resolved;
    for (const auto& host : hosts_) {
        std::unordered_set<std::string> fresh;
        for (auto& ip : resolver_(host)) {
            fresh.insert(ip);
        }
        if (!fresh.empty()) {
            resolved[host] = std::move(fresh);
        }
        // else: this host failed to resolve this round. It's simply absent
        // from `resolved`, so the merge below leaves per_host_ips_[host]
        // untouched — fail-closed if it never resolved before, otherwise
        // keeps the last known-good set for that host.
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [host, ips] : resolved) {
        per_host_ips_[host] = std::move(ips);
    }
}

bool TrustedProxyResolver::isTrusted(const std::string& ip) const {
    if (hosts_.empty()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, ips] : per_host_ips_) {
        if (ips.count(ip) > 0) return true;
    }
    return false;
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
