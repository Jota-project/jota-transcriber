#pragma once
#include <vector>
#include <cstdint>

class AudioDecoder {
public:
    // Decodes encoded audio bytes (OGG/Opus, MP3, WAV, FLAC, M4A, ...)
    // to PCM float32 16kHz mono ready for whisper_full_with_state().
    // Throws std::runtime_error on empty input, unrecognised format, or decode failure.
    static std::vector<float> decode(const std::vector<uint8_t>& data);
};