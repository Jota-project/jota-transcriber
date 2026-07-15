#include <gtest/gtest.h>

#include "whisper/VadGate.h"

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
