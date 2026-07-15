#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <mutex>

// Resolves operator-configured hostnames to IPs and answers whether a given
// IP belongs to a trusted proxy. Deploy-agnostic: works with k8s CoreDNS
// Service FQDNs and with docker-compose / localhost.
//
// Empty host list => enabled() == false and isTrusted() always false (no-op).
// Fail-closed: if a hostname never resolves, nobody is trusted. On a later
// resolution failure the last known-good IP set is kept.
//
// Dual-stack caveat: if the server binds to "::", Boost.Asio reports IPv4
// peers as "::ffff:a.b.c.d", but defaultResolver() returns bare IPv4
// strings (e.g. "a.b.c.d") from AF_INET records. An IPv4 trusted-proxy
// hostname then won't match and silently fails closed (not a security
// hole, but worth knowing when troubleshooting).
//
// Concurrency model: refresh() does the (potentially slow) DNS I/O and does
// NOT hold `mutex_` while calling `resolver_` — only while merging the
// freshly resolved data into `per_host_ips_`. isTrusted() is a pure,
// non-blocking cache read (lock + set lookup) and is safe to call
// concurrently with refresh() from a different thread. In production,
// isTrusted() runs on the accept-loop thread while a separate background
// thread calls refresh() on a timer (see server.cpp) — this class does not
// own or manage that thread itself.
class TrustedProxyResolver {
public:
    using ResolveFn = std::function<std::vector<std::string>(const std::string&)>;

    // resolver defaults to defaultResolver() when empty.
    TrustedProxyResolver(const std::string& comma_separated_hosts,
                         ResolveFn resolver = {});

    // Re-resolves every configured host and merges successes into the
    // cached set. Safe to call from any thread; never blocks isTrusted().
    void refresh();

    // Pure cache read: true if `ip` belongs to any trusted host's last
    // successfully resolved IP set. Never does I/O, never blocks.
    bool isTrusted(const std::string& ip) const;

    bool enabled() const;

    // getaddrinfo-backed resolver (IPv4 + IPv6). Returns empty on failure.
    static std::vector<std::string> defaultResolver(const std::string& host);

private:
    std::vector<std::string> hosts_;
    ResolveFn resolver_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> per_host_ips_;
};
