#include <gtest/gtest.h>
#include "whisper/InferenceLimiter.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

// El InferenceLimiter es un singleton — cada test lo resetea a un estado conocido.
class InferenceLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        InferenceLimiter::instance().setMaxConcurrency(4);
    }
};

TEST_F(InferenceLimiterTest, HasCapacityWhenNoSlotsUsed) {
    EXPECT_TRUE(InferenceLimiter::instance().hasCapacity());
}

TEST_F(InferenceLimiterTest, GuardAcquiresSlotAndReleasesOnDestruction) {
    {
        InferenceLimiter::Guard g;
        // Slot en uso, pero con límite 4 aún hay capacidad
        EXPECT_TRUE(InferenceLimiter::instance().hasCapacity());
    }
    // Guard destruido → slot liberado
    EXPECT_TRUE(InferenceLimiter::instance().hasCapacity());
}

TEST_F(InferenceLimiterTest, NoCapacityWhenAtLimit) {
    InferenceLimiter::instance().setMaxConcurrency(1);
    {
        InferenceLimiter::Guard g;
        EXPECT_FALSE(InferenceLimiter::instance().hasCapacity());
    }
    // Liberado
    EXPECT_TRUE(InferenceLimiter::instance().hasCapacity());
}

TEST_F(InferenceLimiterTest, SecondThreadBlocksUntilFirstGuardReleased) {
    InferenceLimiter::instance().setMaxConcurrency(1);

    std::atomic<bool> thread_started{false};
    std::atomic<bool> thread_acquired{false};

    // Ocupar el único slot con un Guard en heap (para poder liberarlo controladamente)
    auto guard = std::make_unique<InferenceLimiter::Guard>();

    std::thread t([&]() {
        thread_started = true;
        InferenceLimiter::Guard g;   // bloqueará hasta que guard se destruya
        thread_acquired = true;
    });

    // Esperar a que el thread esté intentando adquirir
    while (!thread_started) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_FALSE(thread_acquired);  // sigue bloqueado

    guard.reset();  // liberar el slot → desbloquear el thread
    t.join();

    EXPECT_TRUE(thread_acquired);
}

TEST_F(InferenceLimiterTest, MetricsContainExpectedKeys) {
    std::string m = InferenceLimiter::instance().getMetrics();
    EXPECT_NE(m.find("transcription_active_inferences"), std::string::npos);
    EXPECT_NE(m.find("transcription_max_inferences"), std::string::npos);
}
