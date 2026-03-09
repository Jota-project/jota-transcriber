# Changelog

All notable changes to this project will be documented in this file.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html)

---

## [Unreleased]

### Added
- `--auth-token` flag for static local token authentication — constant-time comparison, no external API required. Also available as `AUTH_TOKEN` env var.

### Fixed
- `buffer_overflowed_` flag now resets deterministically in `flushLoop` when inference drains the buffer below the 20s HWM, instead of waiting for the next client audio chunk.

---

## [1.0.0] — 2026-03-09

First stable release.

### Added

**Core transcription**
- Real-time WebSocket audio transcription using whisper.cpp
- Sliding window with semantic segment commit — partials flow continuously, committed text is never re-sent
- Hallucination guard: rejects decoder loops (repeated bigrams, word repetitions, oversized output)
- High-pass filter (IIR, ~80 Hz cutoff) and peak normalization per audio chunk
- `StreamingWhisperEngine`: thread-safe wrapper with per-session `whisper_state`, shared `whisper_context`
- `ModelCache`: singleton with reference counting and configurable TTL unload
- `InferenceLimiter`: semaphore with blocking `acquire()` and non-blocking `try_acquire()`
- Audio buffer high-water mark (20s) — drops incoming chunks and sends a JSON `warning` to the client on first overflow
- Non-blocking inference in `flushLoop` via `try_acquire()` — GPU saturation skips the cycle instead of blocking the session thread

**WebSocket server**
- Plain WebSocket (WS) and WebSocket over TLS (WSS) on the same port
- Authentication: external API with in-memory cache (configurable TTL)
- Per-IP and global connection limits with RAII `ConnectionGuard`
- Session timeout for idle connections (configurable)
- Graceful shutdown: SIGINT/SIGTERM triggers `shutdownAll()`, joins all session threads with watchdog timeout
- `SessionTracker` for coordinated shutdown of all active sessions
- `flushLoop`: dedicated inference thread per session, decoupled from the WebSocket receive loop
- Max binary frame size guard (1 MB) and correct float32 frame validation
- Fixed 3-second rate limiting window on binary frames

**Protocol**
- JSON config message: `language`, `token`, `publish_mqtt`, `vad_thold`
- Binary audio: float32 PCM, 16 kHz, mono
- Server messages: `ready`, `transcription` (partial/final), `warning`, `error`
- `{"type": "end"}` triggers force-commit and final transcription

**Operations**
- `/health` — always 200 if the process is alive
- `/ready` — 200 or 503 based on `InferenceLimiter` capacity
- `/metrics` — Prometheus text format (active inferences, connections, model load state)
- MQTT publishing of final transcriptions (optional, per-session via `publish_mqtt` config field)
- `.env` file loading at startup
- Docker with NVIDIA GPU support (`docker-compose.yml`)

### Testing
- 68 unit tests across 7 files, single binary `unit_tests`
- Tests that require a model file auto-skip if absent (`GTEST_SKIP`)
- Extracted `HallucinationGuard.h` and `AudioPreprocessor.h` as standalone testable units

---

[Unreleased]: https://github.com/your-username/transcription-service/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/your-username/transcription-service/releases/tag/v1.0.0
