#include <gtest/gtest.h>
#include "audio/AudioDecoder.h"
#include <cstring>
#include <cstdint>

// Build a minimal valid WAV file in memory (silence).
static std::vector<uint8_t> makeSilentWav(float duration_sec, int sample_rate = 16000) {
    int n_samples = static_cast<int>(duration_sec * sample_rate);
    uint32_t data_bytes = static_cast<uint32_t>(n_samples * 2);

    struct __attribute__((packed)) WavHeader {
        char     riff[4]           = {'R','I','F','F'};
        uint32_t chunk_size;
        char     wave[4]           = {'W','A','V','E'};
        char     fmt[4]            = {'f','m','t',' '};
        uint32_t subchunk1_size    = 16;
        uint16_t audio_format      = 1;   // PCM
        uint16_t num_channels      = 1;
        uint32_t sample_rate_val;
        uint32_t byte_rate;
        uint16_t block_align       = 2;
        uint16_t bits_per_sample   = 16;
        char     data_marker[4]    = {'d','a','t','a'};
        uint32_t subchunk2_size;
    };

    WavHeader hdr;
    hdr.chunk_size      = 36 + data_bytes;
    hdr.sample_rate_val = static_cast<uint32_t>(sample_rate);
    hdr.byte_rate       = static_cast<uint32_t>(sample_rate * 2);
    hdr.subchunk2_size  = data_bytes;

    std::vector<uint8_t> buf(sizeof(WavHeader) + data_bytes, 0);
    memcpy(buf.data(), &hdr, sizeof(WavHeader));
    return buf;
}

TEST(AudioDecoderTest, DecodesSilentWavReturnsCorrectLength) {
    auto wav = makeSilentWav(0.1f); // 0.1s → 1600 samples at 16kHz
    auto pcm = AudioDecoder::decode(wav);
    ASSERT_FALSE(pcm.empty());
    // Allow ±10% tolerance for resampler rounding
    EXPECT_NEAR(static_cast<int>(pcm.size()), 1600, 160);
}

TEST(AudioDecoderTest, DecodedSamplesAreNormalized) {
    auto wav = makeSilentWav(0.1f);
    auto pcm = AudioDecoder::decode(wav);
    for (float s : pcm) {
        EXPECT_GE(s, -1.0f);
        EXPECT_LE(s,  1.0f);
    }
}

TEST(AudioDecoderTest, ThrowsOnEmptyInput) {
    EXPECT_THROW(AudioDecoder::decode({}), std::runtime_error);
}

TEST(AudioDecoderTest, ThrowsOnGarbageInput) {
    std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02};
    EXPECT_THROW(AudioDecoder::decode(garbage), std::runtime_error);
}

static std::vector<uint8_t> makeStereoWav(float duration_sec,
                                           int sample_rate = 48000) {
    int n_samples   = static_cast<int>(duration_sec * sample_rate);
    uint32_t data_bytes = static_cast<uint32_t>(n_samples * 2 * 2); // 2ch × 2 bytes

    struct __attribute__((packed)) WavHeader {
        char     riff[4]         = {'R','I','F','F'};
        uint32_t chunk_size;
        char     wave[4]         = {'W','A','V','E'};
        char     fmt[4]          = {'f','m','t',' '};
        uint32_t subchunk1_size  = 16;
        uint16_t audio_format    = 1;   // PCM
        uint16_t num_channels    = 2;
        uint32_t sample_rate_val;
        uint32_t byte_rate;
        uint16_t block_align     = 4;   // 2 channels × 2 bytes
        uint16_t bits_per_sample = 16;
        char     data_marker[4]  = {'d','a','t','a'};
        uint32_t subchunk2_size;
    };

    WavHeader hdr;
    hdr.chunk_size      = 36 + data_bytes;
    hdr.sample_rate_val = static_cast<uint32_t>(sample_rate);
    hdr.byte_rate       = static_cast<uint32_t>(sample_rate * 4);
    hdr.subchunk2_size  = data_bytes;

    std::vector<uint8_t> buf(sizeof(WavHeader) + data_bytes, 0);
    memcpy(buf.data(), &hdr, sizeof(WavHeader));
    return buf;
}

TEST(AudioDecoderTest, DecodesStereo48kHzToMono16kHz) {
    // Stereo 48kHz WAV → must be downmixed to mono and resampled to 16kHz
    auto wav = makeStereoWav(0.5f, 48000);
    auto pcm = AudioDecoder::decode(wav);
    ASSERT_FALSE(pcm.empty());
    // 0.5s at 16kHz = 8000 samples ±10%
    EXPECT_NEAR(static_cast<int>(pcm.size()), 8000, 800);
    for (float s : pcm) {
        EXPECT_GE(s, -1.0f);
        EXPECT_LE(s,  1.0f);
    }
}