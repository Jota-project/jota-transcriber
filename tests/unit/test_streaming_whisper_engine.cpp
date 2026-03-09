#include <gtest/gtest.h>
#include "whisper/StreamingWhisperEngine.h"
#include <whisper.h>
#include <filesystem>
#include <thread>
#include <cmath>

#ifndef PROJECT_ROOT
#define PROJECT_ROOT "."
#endif

const std::string ENGINE_MODEL_PATH =
    std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/models/ggml-small.bin";

class StreamingWhisperEngineTest : public ::testing::Test {
protected:
    whisper_context* ctx_ = nullptr;

    void SetUp() override {
        if (!std::filesystem::exists(ENGINE_MODEL_PATH)) {
            GTEST_SKIP() << "Model not found: " << ENGINE_MODEL_PATH;
        }
        whisper_context_params p = whisper_context_default_params();
        p.use_gpu    = true;
        p.flash_attn = false; // CI/CPU safe
        ctx_ = whisper_init_from_file_with_params(ENGINE_MODEL_PATH.c_str(), p);
        if (!ctx_) GTEST_SKIP() << "Failed to load model";
    }

    void TearDown() override {
        if (ctx_) { whisper_free(ctx_); ctx_ = nullptr; }
    }
};

// ─── Construcción ────────────────────────────────────────────────────────────

TEST_F(StreamingWhisperEngineTest, ConstructionWithValidContextSucceeds) {
    ASSERT_NO_THROW({
        StreamingWhisperEngine engine(ctx_);
        EXPECT_TRUE(engine.isReady());
    });
}

TEST(StreamingWhisperEngineBasic, ConstructionWithNullContextThrows) {
    EXPECT_THROW(StreamingWhisperEngine engine(nullptr), std::runtime_error);
}

// ─── Gestión del buffer ──────────────────────────────────────────────────────

TEST_F(StreamingWhisperEngineTest, BufferStartsEmpty) {
    StreamingWhisperEngine engine(ctx_);
    EXPECT_EQ(engine.getBufferSize(), 0u);
}

TEST_F(StreamingWhisperEngineTest, ProcessAudioChunkGrowsBuffer) {
    StreamingWhisperEngine engine(ctx_);
    std::vector<float> chunk(1600, 0.0f);
    engine.processAudioChunk(chunk);
    EXPECT_EQ(engine.getBufferSize(), 1600u);
}

TEST_F(StreamingWhisperEngineTest, ResetClearsBuffer) {
    StreamingWhisperEngine engine(ctx_);
    engine.processAudioChunk(std::vector<float>(8000, 0.0f));
    EXPECT_GT(engine.getBufferSize(), 0u);
    engine.reset();
    EXPECT_EQ(engine.getBufferSize(), 0u);
}

TEST_F(StreamingWhisperEngineTest, BufferTruncatedAt30Seconds) {
    StreamingWhisperEngine engine(ctx_);
    for (int i = 0; i < 35; ++i)
        engine.processAudioChunk(std::vector<float>(16000, 0.1f));
    EXPECT_LE(engine.getBufferSize(), static_cast<size_t>(16000 * 30));
}

TEST_F(StreamingWhisperEngineTest, MassiveSingleChunkTruncatedAt30Seconds) {
    StreamingWhisperEngine engine(ctx_);
    engine.processAudioChunk(std::vector<float>(16000 * 40, 0.1f));
    EXPECT_LE(engine.getBufferSize(), static_cast<size_t>(16000 * 30));
}

// ─── Sliding window ──────────────────────────────────────────────────────────

TEST_F(StreamingWhisperEngineTest, SlidingWindowNoForceDoesNotCommitShortBuffer) {
    StreamingWhisperEngine engine(ctx_);
    engine.setLanguage("es");

    engine.processAudioChunk(std::vector<float>(16000 * 3, 0.0f));

    auto res = engine.transcribeSlidingWindow(false);
    EXPECT_TRUE(res.committed_text.empty());
}

TEST_F(StreamingWhisperEngineTest, ForceCommitClearsBuffer) {
    StreamingWhisperEngine engine(ctx_);
    engine.setLanguage("es");

    engine.processAudioChunk(std::vector<float>(16000 * 3, 0.0f));
    EXPECT_EQ(engine.getBufferSize(), static_cast<size_t>(16000 * 3));

    engine.transcribeSlidingWindow(true); // force commit
    EXPECT_EQ(engine.getBufferSize(), 0u);
}

TEST_F(StreamingWhisperEngineTest, TranscribeEmptyBufferReturnsEmpty) {
    StreamingWhisperEngine engine(ctx_);
    auto res = engine.transcribeSlidingWindow(true);
    EXPECT_TRUE(res.committed_text.empty());
    EXPECT_TRUE(res.partial_text.empty());
}

// ─── Configuración ───────────────────────────────────────────────────────────

TEST_F(StreamingWhisperEngineTest, SetLanguageDoesNotCrash) {
    StreamingWhisperEngine engine(ctx_);
    EXPECT_NO_THROW(engine.setLanguage("en"));
    EXPECT_NO_THROW(engine.setLanguage("es"));
    EXPECT_NO_THROW(engine.setLanguage("auto"));
}

TEST_F(StreamingWhisperEngineTest, SetThreadsDoesNotCrash) {
    StreamingWhisperEngine engine(ctx_);
    EXPECT_NO_THROW(engine.setThreads(1));
    EXPECT_NO_THROW(engine.setThreads(4));
}

TEST_F(StreamingWhisperEngineTest, SetBeamSizeDoesNotCrash) {
    StreamingWhisperEngine engine(ctx_);
    EXPECT_NO_THROW(engine.setBeamSize(1));
    EXPECT_NO_THROW(engine.setBeamSize(5));
}

TEST_F(StreamingWhisperEngineTest, SetInitialPromptDoesNotCrash) {
    StreamingWhisperEngine engine(ctx_);
    EXPECT_NO_THROW(engine.setInitialPrompt("Transcripción en español"));
    EXPECT_NO_THROW(engine.setInitialPrompt(""));
}

// ─── Conversión de formatos ──────────────────────────────────────────────────

TEST(StreamingWhisperEngineBasic, Int16ToFloat32ZeroIsZero) {
    auto res = StreamingWhisperEngine::convertInt16ToFloat32({0});
    EXPECT_FLOAT_EQ(res[0], 0.0f);
}

TEST(StreamingWhisperEngineBasic, Int16ToFloat32PositiveHalf) {
    auto res = StreamingWhisperEngine::convertInt16ToFloat32({16384});
    EXPECT_NEAR(res[0], 0.5f, 0.01f);
}

TEST(StreamingWhisperEngineBasic, Int16ToFloat32NegativeHalf) {
    auto res = StreamingWhisperEngine::convertInt16ToFloat32({-16384});
    EXPECT_NEAR(res[0], -0.5f, 0.01f);
}

TEST(StreamingWhisperEngineBasic, Int16ToFloat32PreservesSize) {
    std::vector<int16_t> input(1000, 100);
    auto res = StreamingWhisperEngine::convertInt16ToFloat32(input);
    EXPECT_EQ(res.size(), 1000u);
}

// ─── Thread safety ───────────────────────────────────────────────────────────

TEST_F(StreamingWhisperEngineTest, ConcurrentProcessAudioChunkDoesNotCrash) {
    StreamingWhisperEngine engine(ctx_);
    const int n_threads = 4;
    std::vector<std::thread> threads;
    for (int t = 0; t < n_threads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 10; ++i)
                engine.processAudioChunk(std::vector<float>(160, 0.05f));
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(engine.getBufferSize(), static_cast<size_t>(n_threads * 10 * 160));
}

// ─── High-water mark ─────────────────────────────────────────────────────────

TEST_F(StreamingWhisperEngineTest, ProcessAudioChunkReturnsFalseWhenBelowHWM) {
    StreamingWhisperEngine engine(ctx_);
    std::vector<float> chunk(1600, 0.0f); // 100ms
    bool overflow = engine.processAudioChunk(chunk);
    EXPECT_FALSE(overflow);
}

TEST_F(StreamingWhisperEngineTest, ProcessAudioChunkReturnsTrueWhenBufferAtHWM) {
    StreamingWhisperEngine engine(ctx_);
    // Fill exactly to HWM: 20s = 320000 samples
    constexpr size_t HWM = 16000 * 20;
    std::vector<float> fill(HWM, 0.0f);
    engine.processAudioChunk(fill);
    // Buffer is at HWM — next chunk must overflow
    std::vector<float> extra(1600, 0.0f);
    bool overflow = engine.processAudioChunk(extra);
    EXPECT_TRUE(overflow);
}

TEST_F(StreamingWhisperEngineTest, ProcessAudioChunkReturnsFalseAfterReset) {
    StreamingWhisperEngine engine(ctx_);
    constexpr size_t HWM = 16000 * 20;
    std::vector<float> fill(HWM, 0.0f);
    engine.processAudioChunk(fill);
    engine.reset(); // drain buffer
    std::vector<float> extra(1600, 0.0f);
    bool overflow = engine.processAudioChunk(extra);
    EXPECT_FALSE(overflow); // buffer cleared, no overflow
}

TEST_F(StreamingWhisperEngineTest, BufferSizeDoesNotGrowBeyondHWMWhenDropping) {
    StreamingWhisperEngine engine(ctx_);
    constexpr size_t HWM = 16000 * 20;
    engine.processAudioChunk(std::vector<float>(HWM, 0.0f));
    EXPECT_EQ(engine.getBufferSize(), HWM);

    // These should be silently dropped
    engine.processAudioChunk(std::vector<float>(16000, 0.0f));
    engine.processAudioChunk(std::vector<float>(16000, 0.0f));

    // Buffer must still be at exactly HWM — not growing
    EXPECT_EQ(engine.getBufferSize(), HWM);
}

TEST_F(StreamingWhisperEngineTest, ProcessAudioChunkReturnsTrueConsecutivelyAtHWM) {
    StreamingWhisperEngine engine(ctx_);
    constexpr size_t HWM = 16000 * 20;
    engine.processAudioChunk(std::vector<float>(HWM, 0.0f));

    // Every subsequent chunk must return true (buffer stays full)
    EXPECT_TRUE(engine.processAudioChunk(std::vector<float>(1600, 0.0f)));
    EXPECT_TRUE(engine.processAudioChunk(std::vector<float>(1600, 0.0f)));
    EXPECT_TRUE(engine.processAudioChunk(std::vector<float>(1600, 0.0f)));
}
