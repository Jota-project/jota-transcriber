# Scalability — Option B Design

**Date:** 2026-03-08

## Problems

**B — Blocking inference under GPU saturation**
`InferenceLimiter::acquire()` blocks the `flushLoop` thread indefinitely when all inference slots are taken. With 4 slots and 8 possible sessions, up to 4 sessions can stall completely, stopping audio processing and response delivery.

**C — Unbounded audio buffer growth**
A client streaming faster than real-time silently fills the 30-second audio buffer. Old audio is dropped without any signal to the client, causing silent data loss with no feedback.

## Solution: Option 1 — try_acquire + buffer warning

### Non-blocking inference (Problem B)

Add `try_acquire()` to `InferenceLimiter`: returns `false` immediately if no slot is available.

In `StreamingSession::flushLoop()`, replace the blocking `InferenceLimiter::Guard` with a non-blocking try:

```
if (!InferenceLimiter::instance().try_acquire()) return; // retry next tick
// ... run inference ...
InferenceLimiter::instance().release();
```

When GPU is saturated, the flush cycle is skipped and retried at the next 200ms tick. No thread ever blocks waiting for inference. The existing blocking `acquire()` and `Guard` stay for any other callers.

### Audio backpressure (Problem C)

Add a high-water mark constant in `StreamingWhisperEngine`: **20 seconds = 320,000 samples** (2/3 of the 30s hard cap).

Change `processAudioChunk()` return type from `void` to `bool`. Returns `true` when the incoming chunk was dropped due to overflow (buffer already above HWM before the chunk arrived).

In `StreamingSession::handleBinaryMessage()`, check the return value and send a JSON warning to the client once per overflow episode:

```json
{"type":"warning","code":"buffer_full","message":"Audio buffer full, dropping incoming audio"}
```

The overflow flag resets when the buffer drains below the HWM — client gets exactly one warning per overflow episode, not one per frame.

## Files

- `src/whisper/InferenceLimiter.h` — add `try_acquire()`
- `src/whisper/StreamingWhisperEngine.h` — update `processAudioChunk()` signature
- `src/whisper/StreamingWhisperEngine.cpp` — add HWM check, return bool
- `src/server/StreamingSession.h` — use `try_acquire()` in flushLoop, send warning on overflow
- `tests/unit/test_inference_limiter.cpp` — 3 new tests for `try_acquire()`
- `tests/unit/test_streaming_whisper_engine.cpp` — 3 new tests for HWM backpressure

## Testing

**InferenceLimiter:**
- `try_acquire()` returns `true` when slots are free
- `try_acquire()` returns `false` when all slots are taken
- After `release()`, `try_acquire()` succeeds again

**StreamingWhisperEngine:**
- `processAudioChunk()` returns `false` when buffer is below HWM
- `processAudioChunk()` returns `true` when buffer exceeds 20s
- Flag resets to `false` after buffer drains below HWM
