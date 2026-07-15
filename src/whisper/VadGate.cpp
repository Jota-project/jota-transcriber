#include "whisper/VadGate.h"

#include <algorithm>
#include <cmath>   // std::lround
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

VadGate::GatedResult VadGate::gate(const std::vector<float>& original) {
    mapping_.clear();
    original_total_samples_ = static_cast<int64_t>(original.size());

    GatedResult out;
    if (original.empty()) return out;  // had_speech == false

    ensureLoaded();  // may throw

    // Own the segments handle via unique_ptr so it is freed on every path.
    auto seg_deleter = [](whisper_vad_segments* s) {
        if (s) whisper_vad_free_segments(s);
    };
    std::unique_ptr<whisper_vad_segments, decltype(seg_deleter)> segments(
        whisper_vad_segments_from_samples(
            vad_ctx_.get(), params_,
            original.data(), static_cast<int>(original.size())),
        seg_deleter);

    if (!segments) {
        throw std::runtime_error("[VadGate] whisper_vad_segments_from_samples failed");
    }

    const int n = whisper_vad_segments_n_segments(segments.get());
    if (n == 0) return out;  // all silence -> had_speech == false

    constexpr int kSampleRate = 16000;
    const int total = static_cast<int>(original.size());
    const int pad = (params_.speech_pad_ms * kSampleRate) / 1000;   // samples each side
    const int gap = (100 * kSampleRate) / 1000;                     // 100 ms synthetic

    auto cs_to_samples = [&](float cs) -> int {
        // segment boundaries are centiseconds (float); clamp into [0, total].
        long v = std::lround((static_cast<double>(cs) / 100.0) * kSampleRate);
        if (v < 0) v = 0;
        if (v > total) v = total;
        return static_cast<int>(v);
    };

    int64_t gated_cursor = 0;
    int prev_seg_end = 0;  // guards against padding-induced overlap (monotonicity)

    for (int i = 0; i < n; ++i) {
        const float t0 = whisper_vad_segments_get_segment_t0(segments.get(), i);
        const float t1 = whisper_vad_segments_get_segment_t1(segments.get(), i);
        int orig_start = cs_to_samples(t0);
        int orig_end   = cs_to_samples(t1);

        // Apply symmetric padding, clamped to the buffer and to the previous
        // segment's end so original time stays strictly monotonic.
        int seg_start = std::max({0, orig_start - pad, prev_seg_end});
        int seg_end   = std::min(total, orig_end + pad);
        if (seg_end <= seg_start) continue;
        prev_seg_end = seg_end;

        const int64_t gated_start = gated_cursor;
        // Bounds-checked iterator range copy — no raw pointer arithmetic.
        out.samples.insert(out.samples.end(),
                           original.begin() + seg_start,
                           original.begin() + seg_end);
        gated_cursor += (seg_end - seg_start);
        const int64_t gated_end = gated_cursor;

        mapping_.push_back({gated_start, gated_end,
                            static_cast<int64_t>(seg_start),
                            static_cast<int64_t>(seg_end)});

        // Synthetic silence between (not after) segments so Whisper still sees
        // a boundary but the long original silence is gone.
        if (i < n - 1) {
            out.samples.insert(out.samples.end(), static_cast<size_t>(gap), 0.0f);
            gated_cursor += gap;
        }
    }

    out.had_speech = !out.samples.empty();
    return out;
}
