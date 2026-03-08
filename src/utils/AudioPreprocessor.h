#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

namespace AudioPreprocessor {

/**
 * High-pass IIR filter (alpha=0.969, cutoff ~80Hz @ 16kHz) + peak normalization.
 *
 * Modifies pcm in-place. prev_raw and prev_filtered carry filter state between
 * consecutive calls (per-session state — do NOT use static variables).
 *
 * Normalization rules:
 *   peak <= 0.02  → no-op (silence / mic noise, would amplify artifacts)
 *   peak >= 0.9   → no-op (already loud enough)
 *   otherwise     → gain = min(0.9 / peak, 4.0)
 */
inline void process(std::vector<float>& pcm, float& prev_raw, float& prev_filtered) {
    const float alpha = 0.969f;
    float peak = 0.0f;

    for (size_t i = 0; i < pcm.size(); ++i) {
        float raw      = pcm[i];
        float filtered = alpha * (prev_filtered + raw - prev_raw);
        prev_raw       = raw;
        prev_filtered  = filtered;
        pcm[i]         = filtered;
        peak = std::max(peak, std::abs(filtered));
    }

    if (peak > 0.02f && peak < 0.9f) {
        float gain = std::min(0.9f / peak, 4.0f);
        for (float& s : pcm) s *= gain;
    }
}

} // namespace AudioPreprocessor
