#pragma once

#include <whisper.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Silero-VAD silence gate in front of Whisper decoding.
 *
 * Runs whisper.cpp's public Silero VAD over a PCM snapshot and returns a
 * trimmed buffer with long silences (>= min_silence_duration_ms) removed,
 * plus a mapping that lets callers translate a timestamp in the trimmed
 * ("gated") timeline back to a sample offset in the original buffer.
 *
 * Ownership: the whisper_vad_context is owned via a unique_ptr with a custom
 * deleter (RAII). Non-copyable AND non-movable — hold it via unique_ptr.
 *
 * Not thread-safe: one instance per session, called only from that session's
 * inference path.
 */
class VadGate {
public:
    /// One anchor: a contiguous speech region copied into the gated buffer.
    /// All fields are sample offsets (16 kHz).
    struct SegmentMapping {
        int64_t gated_start;
        int64_t gated_end;
        int64_t original_start;
        int64_t original_end;
    };

    struct GatedResult {
        std::vector<float> samples;   ///< buffer to feed to whisper (trimmed)
        bool had_speech = false;      ///< false => skip decoding this pass
    };

    VadGate(std::string model_path, whisper_vad_params params, int n_threads);
    ~VadGate();

    /// Convenience factory: builds whisper_vad_params from primitive fields
    /// (the same fields ServerConfig exposes as vad_*) and constructs a
    /// VadGate. Avoids duplicating the whisper_vad_params mapping at every
    /// call site (StreamingWhisperEngine, HandleTranscribe, ...).
    static std::unique_ptr<VadGate> create(const std::string& model_path,
                                           float threshold,
                                           int min_speech_ms,
                                           int min_silence_ms,
                                           float max_speech_s,
                                           int speech_pad_ms,
                                           float samples_overlap,
                                           int n_threads);

    VadGate(const VadGate&) = delete;
    VadGate& operator=(const VadGate&) = delete;
    VadGate(VadGate&&) = delete;
    VadGate& operator=(VadGate&&) = delete;

    /// Run VAD over `original` and build the trimmed buffer + internal mapping.
    /// @throws std::runtime_error if the VAD model fails to load/run.
    GatedResult gate(const std::vector<float>& original);

    /// Translate a whisper timestamp (centiseconds, gated timeline) to an
    /// offset in the ORIGINAL buffer (samples). Uses the mapping built by the
    /// most recent gate() call. Returns 0 if no mapping is available.
    int64_t toOriginalSamples(int64_t gated_t_centiseconds) const;

    /// Pure, model-free translation used by toOriginalSamples(). Exposed for
    /// testing. `mapping` must be sorted ascending by gated_start.
    static int64_t mapGatedToOriginalSamples(
        const std::vector<SegmentMapping>& mapping,
        int64_t gated_samples,
        int64_t original_total_samples);

private:
    void ensureLoaded();  ///< lazy-load the VAD model on first use

    struct VadCtxDeleter {
        void operator()(whisper_vad_context* c) const {
            if (c) whisper_vad_free(c);
        }
    };

    std::string model_path_;
    whisper_vad_params params_;
    int n_threads_;
    std::unique_ptr<whisper_vad_context, VadCtxDeleter> vad_ctx_;  // null until first gate()
    std::vector<SegmentMapping> mapping_;                          // rebuilt each gate()
    int64_t original_total_samples_ = 0;                           // from last gate()
};
