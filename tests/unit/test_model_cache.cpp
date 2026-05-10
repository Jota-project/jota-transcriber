#include <gtest/gtest.h>
#include "whisper/ModelCache.h"
#include <filesystem>
#include <thread>
#include <chrono>

#ifndef PROJECT_ROOT
#define PROJECT_ROOT "."
#endif

const std::string MODEL_PATH =
    std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/models/ggml-small.bin";

class ModelCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::filesystem::exists(MODEL_PATH)) {
            GTEST_SKIP() << "Model not found: " << MODEL_PATH;
        }
        ModelCache::instance().forceUnload();
        ModelCache::instance().configure(-1); // keep forever durante los tests
    }

    void TearDown() override {
        ModelCache::instance().forceUnload();
        ModelCache::instance().configure(300); // restaurar default
    }
};

TEST_F(ModelCacheTest, AcquireLoadsModel) {
    auto* ctx = ModelCache::instance().acquire(MODEL_PATH);
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(ModelCache::instance().isLoaded());
    EXPECT_EQ(ModelCache::instance().refCount(), 1);
    ModelCache::instance().release();
}

TEST_F(ModelCacheTest, AcquireSamePathReturnsSamePointer) {
    auto* ctx1 = ModelCache::instance().acquire(MODEL_PATH);
    auto* ctx2 = ModelCache::instance().acquire(MODEL_PATH);
    EXPECT_EQ(ctx1, ctx2);              // mismo contexto
    EXPECT_EQ(ModelCache::instance().refCount(), 2);
    ModelCache::instance().release();
    ModelCache::instance().release();
}

TEST_F(ModelCacheTest, ReleaseDecrementsRefCount) {
    ModelCache::instance().acquire(MODEL_PATH);
    ModelCache::instance().acquire(MODEL_PATH);
    EXPECT_EQ(ModelCache::instance().refCount(), 2);
    ModelCache::instance().release();
    EXPECT_EQ(ModelCache::instance().refCount(), 1);
    ModelCache::instance().release();
}

TEST_F(ModelCacheTest, TTLZeroUnloadsImmediately) {
    ModelCache::instance().configure(0);
    ModelCache::instance().acquire(MODEL_PATH);
    ModelCache::instance().release(); // ref_count -> 0, TTL=0 -> unload inmediato
    EXPECT_FALSE(ModelCache::instance().isLoaded());
}

TEST_F(ModelCacheTest, TTLNegativeKeepsModelLoaded) {
    ModelCache::instance().configure(-1);
    ModelCache::instance().acquire(MODEL_PATH);
    ModelCache::instance().release();
    EXPECT_TRUE(ModelCache::instance().isLoaded()); // no debe haber descargado
}

TEST_F(ModelCacheTest, ForceUnloadFreesModel) {
    ModelCache::instance().acquire(MODEL_PATH);
    EXPECT_TRUE(ModelCache::instance().isLoaded());
    ModelCache::instance().forceUnload();
    EXPECT_FALSE(ModelCache::instance().isLoaded());
}

TEST_F(ModelCacheTest, MetricsContainExpectedKeys) {
    ModelCache::instance().acquire(MODEL_PATH);
    std::string m = ModelCache::instance().getMetrics();
    EXPECT_NE(m.find("transcription_model_loaded"), std::string::npos);
    EXPECT_NE(m.find("transcription_model_ref_count"), std::string::npos);
    ModelCache::instance().release();
}

TEST_F(ModelCacheTest, GuardAcquiresAndReleasesOnDestruction) {
    EXPECT_EQ(ModelCache::instance().refCount(), 0);
    {
        ModelCache::Guard guard(MODEL_PATH);
        EXPECT_EQ(ModelCache::instance().refCount(), 1);
        EXPECT_NE(guard.ctx(), nullptr);
    }
    // Guard destroyed → released
    EXPECT_EQ(ModelCache::instance().refCount(), 0);
}

TEST_F(ModelCacheTest, GuardReleasesOnExceptionPath) {
    // Verify the ref count is 0 after a guard scope exits even via exception
    try {
        ModelCache::Guard guard(MODEL_PATH);
        EXPECT_EQ(ModelCache::instance().refCount(), 1);
        throw std::runtime_error("simulated error");
    } catch (...) {}
    EXPECT_EQ(ModelCache::instance().refCount(), 0);
}
