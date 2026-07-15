#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <chrono>

// Resolves operator-configured hostnames to IPs on a refresh interval and
// answers whether a given IP belongs to a trusted proxy. Deploy-agnostic:
// works with k8s CoreDNS Service FQDNs and with docker-compose / localhost.
//
// Empty host list => enabled() == false and isTrusted() always false (no-op).
// Fail-closed: if a hostname never resolves, nobody is trusted. On a later
// resolution failure the last known-good IP set is kept.
//
// isTrusted() is called only from the single-threaded accept loop; the mutex
// is defensive and also lets tests drive it safely.
class TrustedProxyResolver {
public:
    using ResolveFn = std::function<std::vector<std::string>(const std::string&)>;

    // resolver defaults to defaultResolver() when empty.
    TrustedProxyResolver(const std::string& comma_separated_hosts,
                         int refresh_sec,
                         ResolveFn resolver = {});

    bool isTrusted(const std::string& ip);
    bool enabled() const;

    // getaddrinfo-backed resolver (IPv4 + IPv6). Returns empty on failure.
    static std::vector<std::string> defaultResolver(const std::string& host);

private:
    std::vector<std::string> hosts_;
    int refresh_sec_;
    ResolveFn resolver_;

    std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> per_host_ips_;
    std::chrono::steady_clock::time_point last_resolve_{};
    bool ever_resolved_ = false;
};
