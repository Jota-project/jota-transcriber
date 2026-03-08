#include <gtest/gtest.h>
#include "utils/AudioPreprocessor.h"
#include <cmath>
#include <vector>

// Helper: genera una onda senoidal pura (sin => alta frecuencia, el filtro la pasa íntegra)
static std::vector<float> sine(float amplitude, float freq_hz, int samples, int sr = 16000) {
    std::vector<float> v(samples);
    for (int i = 0; i < samples; ++i)
        v[i] = amplitude * sinf(2.0f * M_PI * freq_hz * i / sr);
    return v;
}

static float peakOf(const std::vector<float>& v) {
    float p = 0.0f;
    for (float s : v) p = std::max(p, std::abs(s));
    return p;
}

// Helper: aplica el pipeline sobre una copia y devuelve el resultado
static std::vector<float> apply(std::vector<float> input) {
    float pr = 0.0f, pf = 0.0f;
    AudioPreprocessor::process(input, pr, pf);
    return input;
}

// --- Tamaño de salida ---

TEST(AudioPipeline, OutputSizeEqualsInputSize) {
    auto result = apply(sine(0.3f, 1000.0f, 12345));
    EXPECT_EQ(result.size(), 12345u);
}

// --- Silencio no se normaliza ---

TEST(AudioPipeline, SilenceIsNotAmplified) {
    auto result = apply(std::vector<float>(16000, 0.0f));
    for (float s : result)
        EXPECT_FLOAT_EQ(s, 0.0f);
}

// --- Señal débil se normaliza ---
// 1kHz a amplitud 0.3 → peak ≈ 0.3 → gain = 0.9/0.3 = 3 (< 4) → peak ≈ 0.9

TEST(AudioPipeline, WeakSignalIsNormalizedToNearOne) {
    auto result = apply(sine(0.3f, 1000.0f, 8000));
    float peak = peakOf(result);
    EXPECT_NEAR(peak, 0.9f, 0.05f);
}

// --- Señal fuerte no se toca ---
// 1kHz a amplitud 0.95 → peak ≈ 0.95 (>= 0.9) → no normalization

TEST(AudioPipeline, StrongSignalIsNotNormalized) {
    auto result = apply(sine(0.95f, 1000.0f, 8000));
    float peak = peakOf(result);
    EXPECT_NEAR(peak, 0.95f, 0.03f);
}

// --- Gain se limita a 4x ---
// 1kHz a amplitud 0.05 → peak ≈ 0.05 → gain = 0.9/0.05 = 18 → capped at 4 → peak ≈ 0.2

TEST(AudioPipeline, GainCappedAtFourX) {
    auto result = apply(sine(0.05f, 1000.0f, 8000));
    float peak = peakOf(result);
    // With 4x cap: 0.05 * 4 = 0.2, NOT 0.9
    EXPECT_NEAR(peak, 0.2f, 0.05f);
    EXPECT_LT(peak, 0.5f); // definitely not boosted to 0.9
}

// --- Señal muy pequeña (< 0.02) no se normaliza (umbral anti-ruido) ---
// 1kHz a amplitud 0.01 → peak ≈ 0.01 < 0.02 → no normalization

TEST(AudioPipeline, SubThresholdSignalIsNotNormalized) {
    auto result = apply(sine(0.01f, 1000.0f, 8000));
    float peak = peakOf(result);
    EXPECT_LT(peak, 0.02f); // untouched (still below threshold)
}

// --- Filtro atenúa señal DC ---
// Señal constante → el filtro high-pass la lleva a 0 eventualmente

TEST(AudioPipeline, DCOffsetIsAttenuated) {
    std::vector<float> dc(4000, 0.5f);
    float pr = 0.0f, pf = 0.0f;
    AudioPreprocessor::process(dc, pr, pf);
    // Tras 4000 muestras de DC constante, la salida debe estar muy cerca de 0
    EXPECT_LT(std::abs(dc.back()), 0.02f);
}

// --- El estado del filtro persiste entre llamadas ---

TEST(AudioPipeline, FilterStateUpdatedAfterProcessing) {
    // Verify that filter state variables are updated (not left at zero) after
    // processing a non-zero signal — proves state is carried between calls.
    float pr = 0.0f, pf = 0.0f;
    std::vector<float> chunk = sine(0.3f, 1000.0f, 4000);
    AudioPreprocessor::process(chunk, pr, pf);

    // After processing 4000 samples of a 1kHz tone, state must be non-zero
    EXPECT_NE(pr, 0.0f);
    EXPECT_NE(pf, 0.0f);

    // State from call 1 is used in call 2: processing DC after a tone
    // should produce a different result than processing DC from scratch.
    std::vector<float> dc(400, 0.1f);
    std::vector<float> dc_fresh = dc;
    float pr0 = 0.0f, pf0 = 0.0f;
    AudioPreprocessor::process(dc_fresh, pr0, pf0);
    AudioPreprocessor::process(dc, pr, pf);
    // Outputs differ because initial filter states differ
    EXPECT_NE(dc[0], dc_fresh[0]);
}
