#include <gtest/gtest.h>
#include "server/ConnectionLimiter.h"
#include "server/ConnectionGuard.h"
#include <memory>

// ─── ConnectionLimiter ───────────────────────────────────────────────────────

TEST(ConnectionLimiter, AcceptsConnectionsUpToGlobalLimit) {
    auto lim = std::make_shared<ConnectionLimiter>(2, 10);
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1"));
    EXPECT_TRUE(lim->tryAcquire("1.1.1.2"));
    lim->release("1.1.1.1");
    lim->release("1.1.1.2");
}

TEST(ConnectionLimiter, RejectsWhenGlobalLimitReached) {
    auto lim = std::make_shared<ConnectionLimiter>(2, 10);
    lim->tryAcquire("1.1.1.1");
    lim->tryAcquire("1.1.1.2");
    EXPECT_FALSE(lim->tryAcquire("1.1.1.3"));
    lim->release("1.1.1.1");
    lim->release("1.1.1.2");
}

TEST(ConnectionLimiter, RejectsWhenPerIPLimitReached) {
    auto lim = std::make_shared<ConnectionLimiter>(10, 2);
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1"));
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1"));
    EXPECT_FALSE(lim->tryAcquire("1.1.1.1")); // misma IP, límite 2
    lim->release("1.1.1.1");
    lim->release("1.1.1.1");
}

TEST(ConnectionLimiter, PerIPLimitIsIndependentPerIP) {
    auto lim = std::make_shared<ConnectionLimiter>(10, 1);
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1"));
    EXPECT_TRUE(lim->tryAcquire("1.1.1.2")); // otra IP, límite independiente
    EXPECT_FALSE(lim->tryAcquire("1.1.1.1")); // misma IP bloqueada
    lim->release("1.1.1.1");
    lim->release("1.1.1.2");
}

TEST(ConnectionLimiter, ReleaseRestoresCapacity) {
    auto lim = std::make_shared<ConnectionLimiter>(1, 10);
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1"));
    EXPECT_FALSE(lim->tryAcquire("1.1.1.2"));
    lim->release("1.1.1.1");
    EXPECT_TRUE(lim->tryAcquire("1.1.1.2"));
    lim->release("1.1.1.2");
}

TEST(ConnectionLimiter, MetricsContainExpectedKey) {
    auto lim = std::make_shared<ConnectionLimiter>(8, 2);
    std::string m = lim->getMetrics();
    EXPECT_NE(m.find("transcription_active_connections"), std::string::npos);
}

TEST(ConnectionLimiter, TrustedIpBypassesPerIpLimit) {
    auto lim = std::make_shared<ConnectionLimiter>(10, 1); // per-IP cap 1
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1", true));
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1", true));  // would fail if not trusted
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1", true));
    lim->release("1.1.1.1");
    lim->release("1.1.1.1");
    lim->release("1.1.1.1");
}

TEST(ConnectionLimiter, TrustedIpStillBoundedByGlobalLimit) {
    auto lim = std::make_shared<ConnectionLimiter>(2, 1); // global cap 2
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1", true));
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1", true));
    EXPECT_FALSE(lim->tryAcquire("1.1.1.1", true)); // global cap reached
    lim->release("1.1.1.1");
    lim->release("1.1.1.1");
}

TEST(ConnectionLimiter, UntrustedDefaultStillLimitedPerIp) {
    auto lim = std::make_shared<ConnectionLimiter>(10, 1);
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1"));            // default trusted=false
    EXPECT_FALSE(lim->tryAcquire("1.1.1.1"));           // per-IP cap 1 still applies
    lim->release("1.1.1.1");
}

// ─── ConnectionGuard ─────────────────────────────────────────────────────────

TEST(ConnectionGuard, ReleasesSlotOnDestruction) {
    auto lim = std::make_shared<ConnectionLimiter>(1, 10);
    EXPECT_TRUE(lim->tryAcquire("1.1.1.1")); // adquirir manualmente
    {
        ConnectionGuard guard(lim, "1.1.1.1");
        // Límite global en 1 → no hay más slots
        EXPECT_FALSE(lim->tryAcquire("1.1.1.2"));
    }
    // Guard destruido → liberó el slot de "1.1.1.1"
    EXPECT_TRUE(lim->tryAcquire("1.1.1.2"));
    lim->release("1.1.1.2");
}
