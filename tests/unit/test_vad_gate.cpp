#include <gtest/gtest.h>

#include "whisper/VadGate.h"

#include <cfloat>
#include <filesystem>
#include <whisper.h>

#ifndef PROJECT_ROOT
#define PROJECT_ROOT "."
#endif

static const std::string VAD_MODEL_PATH =
    std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/models/ggml-silero-v5.1.2.bin";

static VadGate makeGate() {
    whisper_vad_params p = whisper_vad_default_params();
    p.threshold               = 0.5f;
    p.min_speech_duration_ms  = 250;
    p.min_silence_duration_ms = 2000;
    p.speech_pad_ms           = 400;
    return VadGate(VAD_MODEL_PATH, p, 4);
}

TEST(VadGateBehavior, FactoryCreatedGateDetectsPureSilenceAsNoSpeech) {
    if (!std::filesystem::exists(VAD_MODEL_PATH)) {
        GTEST_SKIP() << "VAD model not found: " << VAD_MODEL_PATH;
    }
    auto gate = VadGate::create(VAD_MODEL_PATH, 0.5f, 250, 2000, FLT_MAX, 400, 0.1f, 4);
    ASSERT_NE(gate, nullptr);
    std::vector<float> silence(16000 * 4, 0.0f);  // 4 s of zeros
    auto result = gate->gate(silence);
    EXPECT_FALSE(result.had_speech);
    EXPECT_TRUE(result.samples.empty());
}

TEST(VadGateBehavior, EmptyInputHasNoSpeech) {
    VadGate gate = makeGate();
    auto result = gate.gate({});
    EXPECT_FALSE(result.had_speech);
    EXPECT_TRUE(result.samples.empty());
}

TEST(VadGateBehavior, PureSilenceHasNoSpeech) {
    if (!std::filesystem::exists(VAD_MODEL_PATH)) {
        GTEST_SKIP() << "VAD model not found: " << VAD_MODEL_PATH;
    }
    VadGate gate = makeGate();
    std::vector<float> silence(16000 * 4, 0.0f);  // 4 s of zeros
    auto result = gate.gate(silence);
    EXPECT_FALSE(result.had_speech);
    EXPECT_TRUE(result.samples.empty());
}

// Mapping fixture: two speech segments in the ORIGINAL timeline:
//   seg A: original [0, 16000)      -> gated [0, 16000)
//   (100 ms = 1600 samples synthetic silence gap in gated timeline)
//   seg B: original [80000, 96000)  -> gated [17600, 33600)
// i.e. 4 s of original silence between A and B was collapsed to 100 ms.
static std::vector<VadGate::SegmentMapping> twoSegmentMapping() {
    return {
        {0,     16000, 0,     16000},
        {17600, 33600, 80000, 96000},
    };
}

TEST(VadGateMapping, InsideFirstSegmentMapsOneToOne) {
    auto m = twoSegmentMapping();
    // gated sample 8000 is halfway through seg A -> original 8000
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, 8000, 96000), 8000);
}

TEST(VadGateMapping, InsideSecondSegmentMapsWithOffset) {
    auto m = twoSegmentMapping();
    // gated 25600 is 8000 into seg B (gated_start 17600) -> original 80000+8000
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, 25600, 96000), 88000);
}

TEST(VadGateMapping, InSyntheticGapMapsToSegmentEnd) {
    auto m = twoSegmentMapping();
    // gated 16800 is inside the 1600-sample gap after seg A -> original end of A
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, 16800, 96000), 16000);
}

TEST(VadGateMapping, PastLastSegmentClampsToLastOriginalEnd) {
    auto m = twoSegmentMapping();
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, 999999, 96000), 96000);
}

TEST(VadGateMapping, NonPositiveMapsToZero) {
    auto m = twoSegmentMapping();
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, 0, 96000), 0);
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, -5, 96000), 0);
}

TEST(VadGateMapping, EmptyMappingReturnsZero) {
    std::vector<VadGate::SegmentMapping> empty;
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(empty, 5000, 96000), 0);
}

// Mapping fixture: leading original silence was trimmed before the first
// speech segment (unlike twoSegmentMapping's seg A, which happens to start
// at original 0 and so can't distinguish "mapped via the mapping" from
// "short-circuited to zero").
//   seg A: original [40000, 60000) -> gated [0, 20000)
static std::vector<VadGate::SegmentMapping> leadingSilenceTrimmedMapping() {
    return {
        {0, 20000, 40000, 60000},
    };
}

TEST(VadGateMapping, GatedZeroWithLeadingSilenceTrimmedMapsToSegmentOriginalStart) {
    auto m = leadingSilenceTrimmedMapping();
    // gated sample 0 is the very start of speech in the gated timeline, but
    // in the original timeline that's after 40000 samples of trimmed leading
    // silence -> must map to 40000, not 0.
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, 0, 96000), 40000);
}

TEST(VadGateMapping, NegativeGatedSamplesStillMapsToZero) {
    auto m = leadingSilenceTrimmedMapping();
    EXPECT_EQ(VadGate::mapGatedToOriginalSamples(m, -5, 96000), 0);
}
