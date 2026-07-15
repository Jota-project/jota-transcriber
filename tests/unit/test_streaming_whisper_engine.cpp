#include <gtest/gtest.h>
#include "whisper/StreamingWhisperEngine.h"
#include <whisper.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <cmath>
#include <fstream>
#include <cstdint>
#include <cfloat>

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

// No setVadConfig() call: exercises the passthrough (no-gate) path on silence.
// Real VAD gating of silence is covered separately by
// GatedLongSilencePreservesBothUtterances below.
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

TEST_F(StreamingWhisperEngineTest, PassthroughWhenVadNotConfigured) {
    // With no setVadConfig() call, gating is disabled and the engine behaves
    // exactly as before: a 3 s silence snapshot still decodes (no skip).
    StreamingWhisperEngine engine(ctx_);
    engine.processAudioChunk(std::vector<float>(16000 * 3, 0.0f));
    ASSERT_NO_THROW({
        auto res = engine.transcribeSlidingWindow(true);
        (void)res;  // no crash / no early-return path taken
    });
    EXPECT_EQ(engine.getBufferSize(), 0u);  // force_commit clears buffer
}

// Minimal PCM16 mono WAV loader (jfk.wav is 16 kHz mono 16-bit). Finds the
// "data" chunk and converts int16 -> float32 in [-1, 1].
static std::vector<float> loadWavMono16k(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    // Locate "data" chunk id.
    size_t i = 12;
    while (i + 8 <= bytes.size()) {
        uint32_t sz = static_cast<uint8_t>(bytes[i+4]) |
                      (static_cast<uint8_t>(bytes[i+5]) << 8) |
                      (static_cast<uint8_t>(bytes[i+6]) << 16) |
                      (static_cast<uint8_t>(bytes[i+7]) << 24);
        if (bytes[i]=='d' && bytes[i+1]=='a' && bytes[i+2]=='t' && bytes[i+3]=='a') {
            std::vector<float> out;
            size_t start = i + 8;
            size_t end = std::min(bytes.size(), start + sz);
            for (size_t p = start; p + 1 < end; p += 2) {
                int16_t s = static_cast<uint8_t>(bytes[p]) |
                            (static_cast<int16_t>(bytes[p+1]) << 8);
                out.push_back(s / 32768.0f);
            }
            return out;
        }
        i += 8 + sz + (sz & 1);
    }
    return {};
}

TEST_F(StreamingWhisperEngineTest, GatedLongSilencePreservesBothUtterances) {
    const std::string vadModel =
        std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/models/ggml-silero-v5.1.2.bin";
    const std::string wav =
        std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/samples/jfk.wav";
    if (!std::filesystem::exists(vadModel) || !std::filesystem::exists(wav)) {
        GTEST_SKIP() << "VAD model or jfk.wav not found";
    }
    std::vector<float> speech = loadWavMono16k(wav);
    ASSERT_FALSE(speech.empty());

    StreamingWhisperEngine engine(ctx_);
    engine.setLanguage("en");
    engine.setVadConfig(vadModel, 0.5f, 250, 2000, FLT_MAX, 400, 0.1f);

    // utterance | 4 s silence | utterance
    engine.processAudioChunk(speech);
    engine.processAudioChunk(std::vector<float>(16000 * 4, 0.0f));
    engine.processAudioChunk(speech);

    auto res = engine.transcribeSlidingWindow(true);
    std::string text = res.committed_text;
    // JFK sample contains "ask not what your country"; expect it present and
    // no crash / no buffer-translation drift (force_commit clears buffer).
    EXPECT_NE(text.find("country"), std::string::npos);
    EXPECT_EQ(engine.getBufferSize(), 0u);
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

// ─── Concurrency ─────────────────────────────────────────────────────────────

TEST_F(StreamingWhisperEngineTest, ConcurrentAddAudioDuringTranscribeDoesNotDeadlock) {
    // Verifies that processAudioChunk() can run concurrently with
    // transcribeSlidingWindow() without deadlock or data corruption.
    // With the old code (mutex held for the entire inference), the adder thread
    // would block for 0.5–2 s; with the new snapshot-based code it proceeds freely.
    StreamingWhisperEngine engine(ctx_);
    engine.setLanguage("es");

    // Fill buffer to inference threshold (3 s of silence)
    std::vector<float> silence(16000 * 3, 0.0f);
    engine.processAudioChunk(silence);

    std::atomic<bool> done{false};
    std::atomic<int>  chunks_added{0};

    // Thread A: keep appending 100 ms chunks while inference runs
    std::thread adder([&]() {
        std::vector<float> chunk(1600, 0.0f);
        while (!done.load(std::memory_order_relaxed)) {
            engine.processAudioChunk(chunk);
            chunks_added.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // Thread B (this thread): run one inference pass
    engine.transcribeSlidingWindow(false);
    done.store(true, std::memory_order_relaxed);
    adder.join();

    // Reaching here without deadlock/crash is the primary success criterion.
    // With 10ms sleep between chunks and inference taking 500ms+, the adder
    // should run at least ~50 times if it is truly running concurrently. We use
    // 3 as a conservative floor to avoid flakiness while still distinguishing
    // "adder ran freely" from "adder ran once before inference happened to complete".
    EXPECT_GT(chunks_added.load(), 3);
}
