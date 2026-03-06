#include <gtest/gtest.h>
#include "whisper/StreamingWhisperEngine.h"
#include <whisper.h>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <thread>

// Ruta al modelo de prueba (usa PROJECT_ROOT definido en CMake)
#ifndef PROJECT_ROOT
#define PROJECT_ROOT "."
#endif

const std::string TEST_MODEL_PATH = std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/models/ggml-base.bin";

/**
 * Helper: load a whisper_context for tests
 */
static whisper_context* loadTestContext() {
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = true;
    cparams.flash_attn = false; // Tests may run on CPU-only machines
    return whisper_init_from_file_with_params(TEST_MODEL_PATH.c_str(), cparams);
}

/**
 * Test Fixture para StreamingWhisperEngine
 */
class StreamingWhisperEngineTest : public ::testing::Test {
protected:
    whisper_context* ctx_ = nullptr;

    void SetUp() override {
        if (!std::filesystem::exists(TEST_MODEL_PATH)) {
            GTEST_SKIP() << "Modelo de prueba no encontrado: " << TEST_MODEL_PATH;
        }
        ctx_ = loadTestContext();
        if (!ctx_) {
            GTEST_SKIP() << "No se pudo cargar el modelo de prueba";
        }
    }

    void TearDown() override {
        if (ctx_) {
            whisper_free(ctx_);
            ctx_ = nullptr;
        }
    }
};

/**
 * Test 1: Verificar que el engine se crea correctamente con un contexto
 */
TEST_F(StreamingWhisperEngineTest, EngineCreatesSuccessfully) {
    ASSERT_NO_THROW({
        StreamingWhisperEngine engine(ctx_);
        EXPECT_TRUE(engine.isReady());
    });
}

/**
 * Test 2: Verificar que falla con contexto nulo
 */
TEST(StreamingWhisperEngineBasicTest, FailsWithNullContext) {
    EXPECT_THROW({
        StreamingWhisperEngine engine(nullptr);
    }, std::runtime_error);
}

/**
 * Test 3: Verificar que el buffer funciona correctamente
 */
TEST_F(StreamingWhisperEngineTest, BufferManagement) {
    StreamingWhisperEngine engine(ctx_);
    
    // Buffer inicial vacío
    EXPECT_EQ(engine.getBufferSize(), 0);
    
    // Agregar audio
    std::vector<float> audio_chunk(1000, 0.5f);
    engine.processAudioChunk(audio_chunk);
    EXPECT_EQ(engine.getBufferSize(), 1000);
    
    // Agregar más audio
    engine.processAudioChunk(audio_chunk);
    EXPECT_EQ(engine.getBufferSize(), 2000);
    
    // Reset
    engine.reset();
    EXPECT_EQ(engine.getBufferSize(), 0);
}

/**
 * Test 4: Verificar conversión de int16 a float32
 */
TEST(StreamingWhisperEngineBasicTest, Int16ToFloat32Conversion) {
    std::vector<int16_t> pcm16 = {0, 16384, -16384, 32767, -32768};
    std::vector<float> pcm32 = StreamingWhisperEngine::convertInt16ToFloat32(pcm16);
    
    ASSERT_EQ(pcm16.size(), pcm32.size());
    
    EXPECT_FLOAT_EQ(pcm32[0], 0.0f);
    EXPECT_NEAR(pcm32[1], 0.5f, 0.01f);
    EXPECT_NEAR(pcm32[2], -0.5f, 0.01f);
    EXPECT_NEAR(pcm32[3], 1.0f, 0.01f);
    EXPECT_NEAR(pcm32[4], -1.0f, 0.01f);
}

/**
 * Test 5: Verificar conversión de bytes a float32
 */
TEST(StreamingWhisperEngineBasicTest, BytesToFloat32Conversion) {
    std::vector<uint8_t> bytes = {
        0x00, 0x00,  // 0
        0x00, 0x40,  // 16384
        0x00, 0xC0,  // -16384
    };
    
    std::vector<float> pcm32 = StreamingWhisperEngine::convertBytesToFloat32(bytes);
    
    EXPECT_EQ(pcm32.size(), 3);
    EXPECT_FLOAT_EQ(pcm32[0], 0.0f);
    EXPECT_NEAR(pcm32[1], 0.5f, 0.01f);
    EXPECT_NEAR(pcm32[2], -0.5f, 0.01f);
}

/**
 * Test 6: Verificar configuración de idioma
 */
TEST_F(StreamingWhisperEngineTest, LanguageConfiguration) {
    StreamingWhisperEngine engine(ctx_);
    
    EXPECT_NO_THROW(engine.setLanguage("en"));
    EXPECT_NO_THROW(engine.setLanguage("es"));
    EXPECT_NO_THROW(engine.setLanguage("auto"));
}

/**
 * Test 7: Verificar configuración de threads
 */
TEST_F(StreamingWhisperEngineTest, ThreadConfiguration) {
    StreamingWhisperEngine engine(ctx_);
    
    EXPECT_NO_THROW(engine.setThreads(1));
    EXPECT_NO_THROW(engine.setThreads(4));
    EXPECT_NO_THROW(engine.setThreads(8));
}

/**
 * Test 8: Verificar transcripción con buffer vacío
 */
TEST_F(StreamingWhisperEngineTest, TranscribeEmptyBuffer) {
    StreamingWhisperEngine engine(ctx_);
    
    std::string result = engine.transcribe();
    EXPECT_EQ(result, "");
}

/**
 * Test 9: Verificar transcripción con audio de silencio
 */
TEST_F(StreamingWhisperEngineTest, TranscribeSilence) {
    StreamingWhisperEngine engine(ctx_);
    engine.setLanguage("es");
    
    // 1 segundo de silencio @ 16kHz
    std::vector<float> silence(16000, 0.0f);
    engine.processAudioChunk(silence);
    
    std::string result = engine.transcribe();
    EXPECT_LE(result.length(), 50);
}

/**
 * Test 10: Verificar transcripción con audio sintético (tono)
 */
TEST_F(StreamingWhisperEngineTest, TranscribeSyntheticTone) {
    StreamingWhisperEngine engine(ctx_);
    engine.setLanguage("es");
    
    const int sample_rate = 16000;
    const float frequency = 440.0f;
    const float duration = 1.0f;
    const int num_samples = static_cast<int>(sample_rate * duration);
    
    std::vector<float> tone(num_samples);
    for (int i = 0; i < num_samples; ++i) {
        tone[i] = 0.3f * std::sin(2.0f * M_PI * frequency * i / sample_rate);
    }
    
    engine.processAudioChunk(tone);
    
    EXPECT_NO_THROW({
        std::string result = engine.transcribe();
    });
}

/**
 * Test 11: Verificar thread-safety con múltiples threads
 */
TEST_F(StreamingWhisperEngineTest, ThreadSafety) {
    StreamingWhisperEngine engine(ctx_);
    
    const int num_threads = 4;
    const int chunks_per_thread = 10;
    std::vector<std::thread> threads;
    
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&engine, chunks_per_thread]() {
            for (int i = 0; i < chunks_per_thread; ++i) {
                std::vector<float> chunk(100, 0.1f);
                engine.processAudioChunk(chunk);
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(engine.getBufferSize(), num_threads * chunks_per_thread * 100);
}

/**
 * Test 12: Verificar overflow del buffer
 */
TEST_F(StreamingWhisperEngineTest, BufferOverflow) {
    StreamingWhisperEngine engine(ctx_);
    
    const int sample_rate = 16000;
    const int seconds = 35;
    const int chunk_size = sample_rate;
    
    for (int i = 0; i < seconds; ++i) {
        std::vector<float> chunk(chunk_size, 0.1f);
        engine.processAudioChunk(chunk);
    }
    
    EXPECT_LE(engine.getBufferSize(), sample_rate * 30);
}

/**
 * Test 13: Verificar reset después de transcripción
 */
TEST_F(StreamingWhisperEngineTest, ResetAfterTranscription) {
    StreamingWhisperEngine engine(ctx_);
    
    std::vector<float> audio(16000, 0.1f);
    engine.processAudioChunk(audio);
    EXPECT_GT(engine.getBufferSize(), 0);
    
    engine.transcribe();
    
    engine.reset();
    EXPECT_EQ(engine.getBufferSize(), 0);
    
    engine.processAudioChunk(audio);
    EXPECT_EQ(engine.getBufferSize(), 16000);
}

/**
 * Test 14: Verificar chunk masivo
 */
TEST_F(StreamingWhisperEngineTest, MassiveSingleChunk) {
    StreamingWhisperEngine engine(ctx_);
    
    const int massive_size = 16000 * 40;
    std::vector<float> massive_chunk(massive_size, 0.1f);
    
    engine.processAudioChunk(massive_chunk);
    
    EXPECT_LE(engine.getBufferSize(), 16000 * 30);
}

/**
 * Test 15: Verificar beam size configuration
 */
TEST_F(StreamingWhisperEngineTest, BeamSizeConfiguration) {
    StreamingWhisperEngine engine(ctx_);
    
    EXPECT_NO_THROW(engine.setBeamSize(1));
    EXPECT_NO_THROW(engine.setBeamSize(5));
    EXPECT_NO_THROW(engine.setBeamSize(10));
}

/**
 * Test 16: Verificar initial prompt
 */
TEST_F(StreamingWhisperEngineTest, InitialPromptConfiguration) {
    StreamingWhisperEngine engine(ctx_);
    
    EXPECT_NO_THROW(engine.setInitialPrompt("Transcripción en español"));
    EXPECT_NO_THROW(engine.setInitialPrompt(""));
}

/**
 * Test 17: Verificar audio con valores NaN/Inf
 */
TEST_F(StreamingWhisperEngineTest, AudioWithNaN) {
    StreamingWhisperEngine engine(ctx_);
    
    std::vector<float> bad_audio(16000);
    bad_audio[0] = std::numeric_limits<float>::quiet_NaN();
    bad_audio[1] = std::numeric_limits<float>::infinity();
    
    engine.processAudioChunk(bad_audio);
    
    EXPECT_NO_THROW({
        engine.transcribe();
    });
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
