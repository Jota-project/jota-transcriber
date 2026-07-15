#include <gtest/gtest.h>
#include "server/TrustedProxyResolver.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

TEST(TrustedProxyResolver, EmptyHostsDisabled) {
    TrustedProxyResolver r("");
    EXPECT_FALSE(r.enabled());
    EXPECT_FALSE(r.isTrusted("127.0.0.1"));
}

TEST(TrustedProxyResolver, ResolvesAndTrustsBothFamilies) {
    TrustedProxyResolver r("localhost",
        [](const std::string&) -> std::vector<std::string> {
            return {"127.0.0.1", "::1"};
        });
    EXPECT_TRUE(r.enabled());
    r.refresh();
    EXPECT_TRUE(r.isTrusted("127.0.0.1"));
    EXPECT_TRUE(r.isTrusted("::1"));
    EXPECT_FALSE(r.isTrusted("8.8.8.8"));
}

TEST(TrustedProxyResolver, FailClosedWhenNeverResolved) {
    TrustedProxyResolver r("gw",
        [](const std::string&) -> std::vector<std::string> { return {}; });
    EXPECT_TRUE(r.enabled());              // configured...
    r.refresh();                           // ...but the resolver returns nothing...
    EXPECT_FALSE(r.isTrusted("10.0.0.1")); // ...so nothing is trusted (fail-closed)
}

// Replaces the old CachesWithinTtl test: there is no TTL left in the class,
// so the thing worth proving is the new contract itself — isTrusted() must
// never trigger a resolution, no matter how many times it's called.
TEST(TrustedProxyResolver, IsTrustedDoesNotTriggerResolution) {
    std::atomic<int> calls{0};
    TrustedProxyResolver r("gw",
        [&calls](const std::string&) -> std::vector<std::string> {
            ++calls;
            return {"10.0.0.1"};
        });
    EXPECT_FALSE(r.isTrusted("10.0.0.1"));
    EXPECT_FALSE(r.isTrusted("10.0.0.1"));
    EXPECT_FALSE(r.isTrusted("10.0.0.1"));
    EXPECT_EQ(calls.load(), 0);
}

// Replaces the old ReresolvesAfterTtl test: refresh() has no internal TTL or
// caching of its own — every call re-resolves. Cadence is the caller's job.
TEST(TrustedProxyResolver, RefreshAlwaysCallsResolver) {
    std::atomic<int> calls{0};
    TrustedProxyResolver r("gw",
        [&calls](const std::string&) -> std::vector<std::string> {
            ++calls;
            return {"10.0.0.1"};
        });
    r.refresh();
    r.refresh();
    EXPECT_EQ(calls.load(), 2);
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
}

TEST(TrustedProxyResolver, KeepsLastGoodOnResolutionFailure) {
    std::atomic<int> calls{0};
    TrustedProxyResolver r("gw",
        [&calls](const std::string&) -> std::vector<std::string> {
            // First call succeeds, later calls fail (empty).
            return (++calls == 1) ? std::vector<std::string>{"10.0.0.1"}
                                  : std::vector<std::string>{};
        });
    r.refresh();                          // call 1: good set stored
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    r.refresh();                          // call 2: failure -> keep old set
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
}

// Regression test: a multi-host CSV list must not let one host's transient
// resolution failure evict another host's previously-good IPs.
TEST(TrustedProxyResolver, MultiHostPartialFailureKeepsOtherHostGoodIps) {
    std::atomic<int> hostBCalls{0};
    TrustedProxyResolver r("hostA,hostB",
        [&hostBCalls](const std::string& host) -> std::vector<std::string> {
            if (host == "hostA") {
                return {"10.0.0.1"};  // hostA always resolves successfully
            }
            // hostB: succeeds on first call, fails (empty) afterwards.
            return (++hostBCalls == 1) ? std::vector<std::string>{"10.0.0.2"}
                                        : std::vector<std::string>{};
        });

    // First refresh: both hosts resolve successfully -> both IPs trusted.
    r.refresh();
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_TRUE(r.isTrusted("10.0.0.2"));

    // Second refresh: hostB now fails. hostA's IP must remain trusted, and
    // hostB's previously-good IP must NOT be evicted just because hostB
    // failed this round.
    r.refresh();
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
    TrustedProxyResolver r(" hostA , , hostB ",
        [](const std::string& host) -> std::vector<std::string> {
            if (host == "hostA") return {"10.0.0.1"};
            if (host == "hostB") return {"10.0.0.2"};
            return {};  // untrimmed host string (e.g. " hostA ") would land here
        });
    EXPECT_TRUE(r.enabled());
    r.refresh();
    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
    EXPECT_TRUE(r.isTrusted("10.0.0.2"));
}

// A CSV string that is only whitespace/commas has no real hosts -> disabled,
// same as an empty string.
TEST(TrustedProxyResolver, AllWhitespaceOrEmptySegmentsCsvDisabled) {
    TrustedProxyResolver r(" , ,  ");
    EXPECT_FALSE(r.enabled());
    EXPECT_FALSE(r.isTrusted("127.0.0.1"));
}

// Regression test for #73: refresh() must not hold the internal lock while
// the resolver is doing its (potentially slow/blocking) I/O — a resolver
// stuck inside a DNS call must never make isTrusted() block. This test
// fails against the old implementation (mutex held for the whole resolver_
// call) and passes once refresh() resolves outside the lock.
TEST(TrustedProxyResolver, RefreshDoesNotBlockConcurrentIsTrusted) {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool release_resolver = false;
    std::atomic<bool> resolver_entered{false};

    TrustedProxyResolver r("gw",
        [&](const std::string&) -> std::vector<std::string> {
            resolver_entered = true;
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait(lock, [&] { return release_resolver; });
            return {"10.0.0.1"};
        });

    std::thread refresher([&r] { r.refresh(); });

    // Wait until refresh() is confirmed stuck inside the resolver.
    while (!resolver_entered.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // While refresh() is blocked inside the resolver, isTrusted() must
    // return immediately rather than waiting for it.
    EXPECT_FALSE(r.isTrusted("10.0.0.1"));

    // Release the resolver and let refresh() finish.
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_resolver = true;
    }
    gate_cv.notify_one();
    refresher.join();

    EXPECT_TRUE(r.isTrusted("10.0.0.1"));
}
