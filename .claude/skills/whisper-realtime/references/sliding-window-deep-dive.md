# Sliding Window Deep Dive

Detailed analysis of `transcribeSlidingWindow()` in `StreamingWhisperEngine.cpp`.

## Algorithm Overview

```
INPUTS:
  audio_buffer_     — accumulated PCM samples (float32, 16kHz)
  force_commit      — if true, flush everything to committed_text

OUTPUTS:
  TranscribeResult.committed_text  — stable text, safe to display permanently
  TranscribeResult.partial_text    — in-flight text, may change on next call

SIDE EFFECT:
  audio_buffer_ is trimmed (leading samples erased) when commit happens
```

## Step-by-Step Execution

### Step 1: Run whisper on entire buffer

```cpp
int result = whisper_full_with_state(ctx_, state_, params,
    audio_buffer_.data(), audio_buffer_.size());
// Processes ALL audio in buffer, every call
// This is the O(N²) culprit — must keep buffer short
```

### Step 2: Decide commit boundary

```cpp
const size_t max_window_samples = 16000 * 10;  // 10s threshold

if (force_commit || audio_buffer_.size() >= max_window_samples) {
    // Find segment that ends before "safe zone" (last 2s)
    int64_t overlap_bounds_t = (static_cast<int64_t>(audio_buffer_.size()) - 32000) / 160;
    // 32000 samples = 2s overlap
    // Divide by 160: convert samples to whisper timestamp units (10ms)
    
    // Walk segments backwards to find commit point
    for (int i = n_segments - 1; i >= 0; --i) {
        int64_t t1 = whisper_full_get_segment_t1_from_state(state_, i);
        if (t1 < overlap_bounds_t) {
            commit_up_to_segment = i;
            commit_t1 = t1;
            break;
        }
    }
}
```

### Step 3: Erase committed audio

```cpp
size_t samples_to_erase = commit_t1 * 160;
audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + samples_to_erase);
```

The remaining `audio_buffer_` starts from `commit_t1` (in whisper's time reference).
**Critical**: Next call to `whisper_full_with_state()` will process this remaining audio,
but whisper will assign timestamps starting from 0 again — not from `commit_t1`.
This is correct behavior: timestamps are always relative to the start of the passed buffer.

## Failure Modes

### Failure Mode 1: commit_t1 == 0

**When**: whisper returns a segment where `t1 = 0`. Common causes:
- Buffer too short (< 1s) — whisper can't compute reliable timestamps
- `no_context = false` — decoder uses previous context and collapses timestamps
- Greedy decoding with very noisy audio

**Result**: `samples_to_erase = 0 * 160 = 0` → erase does nothing → buffer never shrinks

**Detection**:
```cpp
if (commit_t1 == 0 && commit_up_to_segment >= 0) {
    Log::warn("commit_t1=0, buffer not trimmed. Segments: " + std::to_string(n_segments) +
              " buf=" + std::to_string(audio_buffer_.size() / 16000.0f) + "s");
}
```

**Fix**: Force-erase a fraction of the buffer when commit_t1 == 0:
```cpp
if (commit_t1 == 0 && audio_buffer_.size() > 32000) {
    // Fallback: erase first half of buffer (keep second half as overlap)
    size_t fallback_erase = audio_buffer_.size() / 2;
    audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + fallback_erase);
    Log::warn("Fallback erase: " + std::to_string(fallback_erase) + " samples");
}
```

### Failure Mode 2: overlap_bounds_t is negative or zero

**When**: `audio_buffer_.size() < 32000` (less than 2 seconds of audio)

**Calculation**: `(N - 32000) / 160` where N < 32000 → result is negative → no segment
has t1 < negative_number → `commit_up_to_segment` stays -1 → no commit

**Result**: Code falls through to "all text is partial" branch. This is correct behavior
for buffers < 2s. Not a bug.

**But**: If buffer never reaches 2s (audio stops, restart, reconnect), commit never fires.
The `force_commit=true` path in `handleEnd()` handles this correctly.

### Failure Mode 3: Continuous speech > 10s without segment boundaries

**When**: User speaks a long sentence without pause. Whisper may emit a single segment
for the entire 10s buffer. With `n_segments == 1`, the fallback fires:

```cpp
if (commit_up_to_segment == -1 && n_segments > 1) {
    commit_up_to_segment = n_segments - 2;  // ← only fires if n_segments > 1
}
```

If `n_segments == 1`, nothing commits. Buffer hits max (30s), oldest audio is discarded
(correct), but the single-segment problem persists.

**Fix**: Add explicit handling for n_segments == 1 with large buffer:
```cpp
if (commit_up_to_segment == -1 && audio_buffer_.size() >= max_window_samples) {
    // Force commit at 8s mark, keep 2s overlap
    int64_t force_commit_t = ((int64_t)audio_buffer_.size() - 32000) / 160;
    // Find any segment with t1 <= force_commit_t
    // If none, erase up to 8s mark directly
    size_t erase_samples = audio_buffer_.size() - 32000;
    std::string partial_forced;
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(state_, i);
        if (text) partial_forced += text;
    }
    audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + erase_samples);
    res.committed_text = partial_forced;
    return res;
}
```

## Timestamp Arithmetic Reference

```
whisper timestamps are in centiseconds (10ms units)

1 whisper unit = 10ms = 160 samples @ 16kHz

Conversion table:
  t=100  → 1.0s  → 16000 samples
  t=500  → 5.0s  → 80000 samples
  t=1000 → 10.0s → 160000 samples

Formulas:
  samples = timestamp * 160
  seconds = timestamp / 100.0
  timestamp = samples / 160
  timestamp = seconds * 100
```

## Partial vs Committed Text in StreamingSession

```
full_transcription_: accumulated committed text (stable, never changes)
partial_text:        current in-flight transcription (changes each cycle)

Display to user: full_transcription_ + partial_text
```

The merge happens in `processAudioChunk()` and `flushLoop()`:
```cpp
std::string merged_so_far = full_transcription_ + res.partial_text;
json msg = {{"type", "transcription"}, {"text", merged_so_far}, {"is_final", false}};
```

**Risk of duplication**: If `committed_text` is non-empty AND `full_transcription_` already
contains the same text (from a previous cycle where it was partial), you get doubled text.

This shouldn't happen if the erase logic works correctly, because committed text comes from
segments that were erased from the buffer. But if commit_t1 == 0 and the erase fails,
the same segments appear as partial in the next cycle AND get committed in the cycle after,
causing: `full_transcription_ = "Hello world" + partial = "Hello world"` → doubled.