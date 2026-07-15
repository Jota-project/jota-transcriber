#include <gtest/gtest.h>
#include "server/TrustedProxyResolver.h"
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
