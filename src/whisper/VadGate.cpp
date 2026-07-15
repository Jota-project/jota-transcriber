#include "whisper/VadGate.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

VadGate::VadGate(std::string model_path, whisper_vad_params params, int n_threads)
    : model_path_(std::move(model_path)), params_(params), n_threads_(n_threads) {}

VadGate::~VadGate() = default;  // unique_ptr custom deleter frees the context

int64_t VadGate::mapGatedToOriginalSamples(
        const std::vector<SegmentMapping>& mapping,
        int64_t gated_samples,
        int64_t original_total_samples) {
    if (gated_samples <= 0 || mapping.empty()) return 0;

    for (size_t i = 0; i < mapping.size(); ++i) {
        const auto& seg = mapping[i];
        if (gated_samples < seg.gated_start) {
            // Falls before this segment. If i==0 it is before all speech -> 0.
            // Otherwise it is inside the synthetic gap after the previous
            // segment -> map to the previous segment's original end (safe:
            // that boundary only ever discards already-silent original audio).
            return (i == 0) ? 0 : mapping[i - 1].original_end;
        }
        if (gated_samples < seg.gated_end) {
            // Inside this speech segment: 1:1 offset within the segment.
            return seg.original_start + (gated_samples - seg.gated_start);
        }
    }
    // Past the last segment's gated_end -> clamp to last original end.
    int64_t last_end = mapping.back().original_end;
    return std::min(last_end, original_total_samples);
}

int64_t VadGate::toOriginalSamples(int64_t gated_t_centiseconds) const {
    // whisper timestamps are in centiseconds; 1 cs = 160 samples @16kHz.
    const int64_t gated_samples = gated_t_centiseconds * 160;
    return mapGatedToOriginalSamples(mapping_, gated_samples, original_total_samples_);
}

void VadGate::ensureLoaded() {
    if (vad_ctx_) return;
    whisper_vad_context_params cparams = whisper_vad_default_context_params();
    cparams.n_threads = n_threads_;
    cparams.use_gpu   = false;  // Silero is tiny; keep it off the main-model GPU
    whisper_vad_context* raw =
        whisper_vad_init_from_file_with_params(model_path_.c_str(), cparams);
    if (!raw) {
        throw std::runtime_error("[VadGate] Failed to load VAD model: " + model_path_);
    }
    vad_ctx_.reset(raw);
}

VadGate::GatedResult VadGate::gate(const std::vector<float>& /*original*/) {
    // Implemented in Task 2.
    return {};
}
