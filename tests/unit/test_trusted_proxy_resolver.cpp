#include <gtest/gtest.h>
#include "server/TrustedProxyResolver.h"
#include <algorithm>
#include <atomic>

TEST(TrustedProxyResolver, EmptyHostsDisabled) {
    TrustedProxyResolver r("", 30);
    EXPECT_FALSE(r.enabled());
    EXPECT_FALSE(r.isTrusted("127.0.0.1"));
}

TEST(TrustedProxyResolver, ResolvesAndTrustsBothFamilies) {
    TrustedProxyResolver r("localhost", 30,
        [](const std::string&) -> std::vector<std::string> {
            return {"127.0.0.1", "::1"};
        });
    EXPECT_TRUE(r.enabled());
    EXPECT_TRUE(r.isTrusted("127.0.0.1"));
    EXPECT_TRUE(r.isTrusted("::1"));
    EXPECT_FALSE(r.isTrusted("8.8.8.8"));
}

TEST(TrustedProxyResolver, FailClosedWhenNeverResolved) {
    TrustedProxyResolver r("gw", 30,
        [](const std::string&) -> std::vector<std::string> { return {}; });
    EXPECT_TRUE(r.enabled());          // configured...
    EXPECT_FALSE(r.isTrusted("10.0.0.1")); // ...but nothing trusted (fail-closed)
}

TEST(TrustedProxyResolver, CachesWithinTtl) {
    std::atomic<int> calls{0};
    TrustedProxyResolver r("gw", 1000,
        [&calls](const std::string&) -> std::vector<std::string> {
            ++calls;
            return {"10.0.0.1"};
        });
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_EQ(calls.load(), 1); // resolved once, cached within TTL
}

TEST(TrustedProxyResolver, ReresolvesAfterTtl) {
    std::atomic<int> calls{0};
    TrustedProxyResolver r("gw", 0, // 0s => always stale
        [&calls](const std::string&) -> std::vector<std::string> {
            ++calls;
            return {"10.0.0.1"};
        });
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_EQ(calls.load(), 2); // re-resolved each call
}

TEST(TrustedProxyResolver, KeepsLastGoodOnResolutionFailure) {
    std::atomic<int> calls{0};
    TrustedProxyResolver r("gw", 0, // always stale
        [&calls](const std::string&) -> std::vector<std::string> {
            // First call succeeds, later calls fail (empty).
            return (++calls == 1) ? std::vector<std::string>{"10.0.0.1"}
                                  : std::vector<std::string>{};
        });
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));   // call 1: good set stored
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));   // call 2: failure -> keep old set
}

// Regression test: a multi-host CSV list must not let one host's transient
// resolution failure evict another host's previously-good IPs.
TEST(TrustedProxyResolver, MultiHostPartialFailureKeepsOtherHostGoodIps) {
    std::atomic<int> hostBCalls{0};
    TrustedProxyResolver r("hostA,hostB", 0, // always stale
        [&hostBCalls](const std::string& host) -> std::vector<std::string> {
            if (host == "hostA") {
                return {"10.0.0.1"};  // hostA always resolves successfully
            }
            // hostB: succeeds on first call, fails (empty) afterwards.
            return (++hostBCalls == 1) ? std::vector<std::string>{"10.0.0.2"}
                                        : std::vector<std::string>{};
        });

    // First call: both hosts resolve successfully -> both IPs trusted.
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_TRUE(r.isTrusted("10.0.0.2"));

    // Second call: refresh_sec=0 re-triggers resolution; hostB now fails.
    // hostA's IP must remain trusted, and hostB's previously-good IP must
    // NOT be evicted just because hostB failed this round.
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_TRUE(r.isTrusted("10.0.0.2"));
}

// Exercises the real getaddrinfo-backed resolver (no injected ResolveFn).
// "localhost" resolves deterministically via /etc/hosts, no network needed.
TEST(TrustedProxyResolver, DefaultResolverResolvesLocalhost) {
    auto ips = TrustedProxyResolver::defaultResolver("localhost");
    ASSERT_FALSE(ips.empty());
    bool has_loopback = std::find(ips.begin(), ips.end(), "127.0.0.1") != ips.end() ||
                         std::find(ips.begin(), ips.end(), "::1") != ips.end();
    EXPECT_TRUE(has_loopback);
}

TEST(TrustedProxyResolver, DefaultResolverReturnsEmptyForUnresolvableHost) {
    auto ips = TrustedProxyResolver::defaultResolver("this-host-does-not-exist.invalid");
    EXPECT_TRUE(ips.empty());
}

// Whitespace around hosts and empty segments between commas must be
// trimmed/dropped so the resolver queries clean hostnames.
TEST(TrustedProxyResolver, ParsesCsvWithWhitespaceAndEmptySegments) {
    TrustedProxyResolver r(" hostA , , hostB ", 30,
        [](const std::string& host) -> std::vector<std::string> {
            if (host == "hostA") return {"10.0.0.1"};
            if (host == "hostB") return {"10.0.0.2"};
            return {};  // untrimmed host string (e.g. " hostA ") would land here
        });
    EXPECT_TRUE(r.enabled());
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_TRUE(r.isTrusted("10.0.0.2"));
}

// A CSV string that is only whitespace/commas has no real hosts -> disabled,
// same as an empty string.
TEST(TrustedProxyResolver, AllWhitespaceOrEmptySegmentsCsvDisabled) {
    TrustedProxyResolver r(" , ,  ", 30);
    EXPECT_FALSE(r.enabled());
    EXPECT_FALSE(r.isTrusted("127.0.0.1"));
}
