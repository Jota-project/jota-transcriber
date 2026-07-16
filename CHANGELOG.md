## [1.4.1](https://github.com/Jota-project/jota-transcriber/compare/v1.4.0...v1.4.1) (2026-07-16)


### Bug Fixes

* HandshakeWatchdog kills long-running HTTP requests, not just idle handshakes ([#83](https://github.com/Jota-project/jota-transcriber/issues/83)) ([38adc8e](https://github.com/Jota-project/jota-transcriber/commit/38adc8e22ac167ef078609e494eb24a25232c511)), closes [#81](https://github.com/Jota-project/jota-transcriber/issues/81)

# [1.4.0](https://github.com/Jota-project/jota-transcriber/compare/v1.3.4...v1.4.0) (2026-07-16)


### Features

* WebSocket backpressure — flow_control pause/resume + dropped_chunks warning ([#82](https://github.com/Jota-project/jota-transcriber/issues/82)) ([b462b73](https://github.com/Jota-project/jota-transcriber/commit/b462b7373bc95bea5e05dce400094e1455bda167)), closes [hi#water](https://github.com/hi/issues/water) [hi#water](https://github.com/hi/issues/water) [hi#water](https://github.com/hi/issues/water) [hi#water](https://github.com/hi/issues/water) [hi#water](https://github.com/hi/issues/water)

## [1.3.4](https://github.com/Jota-project/jota-transcriber/compare/v1.3.3...v1.3.4) (2026-07-15)


### Bug Fixes

* restore binary name in docker-compose command override ([#80](https://github.com/Jota-project/jota-transcriber/issues/80)) ([f3e2095](https://github.com/Jota-project/jota-transcriber/commit/f3e20956bbc1cb65552b7b81f694135cfb690209))

## [1.3.3](https://github.com/Jota-project/jota-transcriber/compare/v1.3.2...v1.3.3) (2026-07-15)


### Bug Fixes

* Docker build stub list drift + deploy config/docs audit ([#79](https://github.com/Jota-project/jota-transcriber/issues/79)) ([07e985d](https://github.com/Jota-project/jota-transcriber/commit/07e985daba0c8dca78e2a67e87587eaf6e33181f))

## [1.3.2](https://github.com/Jota-project/jota-transcriber/compare/v1.3.1...v1.3.2) (2026-07-15)


### Bug Fixes

* silenciar warnings deprecated de libavcodec en path legacy de AudioDecoder ([#78](https://github.com/Jota-project/jota-transcriber/issues/78)) ([3b20971](https://github.com/Jota-project/jota-transcriber/commit/3b20971782ec9249c82c7590f06301142c95ae90)), closes [#pragma](https://github.com/Jota-project/jota-transcriber/issues/pragma) [#pragma](https://github.com/Jota-project/jota-transcriber/issues/pragma) [#pragma](https://github.com/Jota-project/jota-transcriber/issues/pragma) [#pragma](https://github.com/Jota-project/jota-transcriber/issues/pragma) [#pragma](https://github.com/Jota-project/jota-transcriber/issues/pragma) [#pragma](https://github.com/Jota-project/jota-transcriber/issues/pragma)

## [1.3.1](https://github.com/Jota-project/jota-transcriber/compare/v1.3.0...v1.3.1) (2026-07-15)


### Bug Fixes

* sessions colgadas y límite por IP ([#68](https://github.com/Jota-project/jota-transcriber/issues/68) [#69](https://github.com/Jota-project/jota-transcriber/issues/69)) ([#70](https://github.com/Jota-project/jota-transcriber/issues/70)) ([62f2352](https://github.com/Jota-project/jota-transcriber/commit/62f23525a570f43916bb9441923a8e8cfd62ce02))

# [1.3.0](https://github.com/Jota-project/jota-transcriber/compare/v1.2.0...v1.3.0) (2026-07-15)


### Features

* **vad:** silence gating real con Silero VAD (whisper.cpp) — Hilo C de [#64](https://github.com/Jota-project/jota-transcriber/issues/64) ([#71](https://github.com/Jota-project/jota-transcriber/issues/71)) ([27e392e](https://github.com/Jota-project/jota-transcriber/commit/27e392e01bde0a8e3c3c4d58069f39c12f8f797d))

# [1.2.0](https://github.com/Jota-project/jota-transcriber/compare/v1.1.7...v1.2.0) (2026-07-09)


### Bug Fixes

* bound handleEnd() final decode wait and surface capacity fallback to client ([fe43a83](https://github.com/Jota-project/jota-transcriber/commit/fe43a8329a3519537029ba2d0375d0fda37a8102))


### Features

* add bounded-wait try_acquire_for to InferenceLimiter ([a80da50](https://github.com/Jota-project/jota-transcriber/commit/a80da50299ddb683b900330b18cfb8e2ce9e8f27))
* emit per-session type:status on GPU saturation transitions ([3d699cd](https://github.com/Jota-project/jota-transcriber/commit/3d699cd10d40511bb77b548f0d8f4ff983bf21ea))
* make flushLoop re-inference cadence configurable (flush_min_new_audio_ms) ([2a01cf8](https://github.com/Jota-project/jota-transcriber/commit/2a01cf8386762009f67948c768ef6b0e489ba3b6))

## [1.1.7](https://github.com/Jota-project/jota-transcriber/compare/v1.1.6...v1.1.7) (2026-07-09)


### Bug Fixes

* downgrade CUDA 12.8 → 12.2 for GTX 1060 (Pascal) GPU compatibility ([#63](https://github.com/Jota-project/jota-transcriber/issues/63)) ([d0b63bd](https://github.com/Jota-project/jota-transcriber/commit/d0b63bd53a7d33b6c1e4a2cb26315eb6621a617e))
* downgrade CUDA toolkit from 12.8 to 12.2 for Pascal GPU compatibility ([339b4ce](https://github.com/Jota-project/jota-transcriber/commit/339b4ce479b3a24248af73fd8374765aa2c74a55))

## [1.1.6](https://github.com/Jota-project/jota-transcriber/compare/v1.1.5...v1.1.6) (2026-05-12)


### Bug Fixes

* minor quality — MultipartParser case-insensitive headers, thread_local mt19937 ([#54](https://github.com/Jota-project/jota-transcriber/issues/54)) ([d2d8713](https://github.com/Jota-project/jota-transcriber/commit/d2d8713bf734f6d0357268ecc8b194dbfad3649f)), closes [#46](https://github.com/Jota-project/jota-transcriber/issues/46)

## [1.1.5](https://github.com/Jota-project/jota-transcriber/compare/v1.1.4...v1.1.5) (2026-05-12)


### Bug Fixes

* release buffer_mutex_ during whisper inference (snapshot-based transcribeSlidingWindow) ([#53](https://github.com/Jota-project/jota-transcriber/issues/53)) ([a91cc79](https://github.com/Jota-project/jota-transcriber/commit/a91cc7933b9896292884225dbfd18cb70d8b282c)), closes [#45](https://github.com/Jota-project/jota-transcriber/issues/45)

## [1.1.4](https://github.com/Jota-project/jota-transcriber/compare/v1.1.3...v1.1.4) (2026-05-12)


### Bug Fixes

* InferenceLimiter::TryGuard — eliminate TOCTOU and exception slot leak ([#50](https://github.com/Jota-project/jota-transcriber/issues/50)) ([9f73bfa](https://github.com/Jota-project/jota-transcriber/commit/9f73bfa6323f41fe4bf3c6da4ad931a0a69c8e6f)), closes [#44](https://github.com/Jota-project/jota-transcriber/issues/44)

## [1.1.3](https://github.com/Jota-project/jota-transcriber/compare/v1.1.2...v1.1.3) (2026-05-11)


### Bug Fixes

* ModelCache stability — RAII guard, cancelable timer, destructor safety ([#49](https://github.com/Jota-project/jota-transcriber/issues/49)) ([8059038](https://github.com/Jota-project/jota-transcriber/commit/8059038438614c19f55ba0151fee34fd4e846b8b)), closes [#43](https://github.com/Jota-project/jota-transcriber/issues/43)

## [1.1.2](https://github.com/Jota-project/jota-transcriber/compare/v1.1.1...v1.1.2) (2026-05-10)


### Bug Fixes

* AudioDecoder robustness — null checks, EAGAIN, swr_init, ch_layout version ([#48](https://github.com/Jota-project/jota-transcriber/issues/48)) ([7e0702b](https://github.com/Jota-project/jota-transcriber/commit/7e0702b4fb0fde062b43acaaa261fd239ece1b9d))

## [1.1.1](https://github.com/Jota-project/jota-transcriber/compare/v1.1.0...v1.1.1) (2026-05-10)


### Bug Fixes

* server quick wins — model arg, auth nullptr, verbose_json thread-safety, text/plain trim ([#47](https://github.com/Jota-project/jota-transcriber/issues/47)) ([4ffe3ec](https://github.com/Jota-project/jota-transcriber/commit/4ffe3ecec6e148b5222fd3e083c2025b12c95d8d)), closes [#41](https://github.com/Jota-project/jota-transcriber/issues/41)

# [1.1.0](https://github.com/Jota-project/jota-transcriber/compare/v1.0.1...v1.1.0) (2026-05-10)


### Features

* add POST /v1/audio/transcriptions (OpenAI-compatible) ([#40](https://github.com/Jota-project/jota-transcriber/issues/40)) ([43c2e47](https://github.com/Jota-project/jota-transcriber/commit/43c2e476e2c5d71017d3e75c80b7934c019aaaba)), closes [#31](https://github.com/Jota-project/jota-transcriber/issues/31) [#32](https://github.com/Jota-project/jota-transcriber/issues/32) [#37](https://github.com/Jota-project/jota-transcriber/issues/37)

## [1.0.1](https://github.com/Jota-project/jota-transcriber/compare/v1.0.0...v1.0.1) (2026-04-06)


### Bug Fixes

* eliminar race condition entre flushLoop y handleEnd ([#28](https://github.com/Jota-project/jota-transcriber/issues/28)) ([5c6091e](https://github.com/Jota-project/jota-transcriber/commit/5c6091eaa525b3037b6bd3238930b86bd0632465)), closes [#27](https://github.com/Jota-project/jota-transcriber/issues/27) [#1](https://github.com/Jota-project/jota-transcriber/issues/1) [#2](https://github.com/Jota-project/jota-transcriber/issues/2) [#3](https://github.com/Jota-project/jota-transcriber/issues/3)

# 1.0.0 (2026-03-15)


### Bug Fixes

* add 1MB max frame size guard + correct rate limit comment ([de611a6](https://github.com/Jota-project/jota-transcriber/commit/de611a6ad882eea03120ed16fe64aa6923735f15))
* **buffer:** corregir desbordamiento de buffer y añadir tests ([83cd0cc](https://github.com/Jota-project/jota-transcriber/commit/83cd0cca00ea374e8a5b17c7604c188cf1b2ccfd))
* correct audio frame minimum size check (float32, not WAV header) ([08f29b1](https://github.com/Jota-project/jota-transcriber/commit/08f29b1ba5e7d833f384bdcbea9b857ebba963b3))
* correct client protocol docs and test client Ctrl+C handling ([5c402ca](https://github.com/Jota-project/jota-transcriber/commit/5c402caffcb427d886ae88dd4d46adfd10eb7677))
* graceful shutdown with explicit thread join and configurable timeout ([25a9654](https://github.com/Jota-project/jota-transcriber/commit/25a9654973ba535fd74c0c47ce9f6d04f85a943c))
* move rate limit accounting after frame validation guards ([a030c52](https://github.com/Jota-project/jota-transcriber/commit/a030c5251574888c19822171dfe125ef6a47372b))
* remove stale nullptr mqtt_publisher from StreamingSession test ([9ab5081](https://github.com/Jota-project/jota-transcriber/commit/9ab50813e69c824a771aebc8f74d6f87014819ff))
* reset buffer_overflowed_ in flushLoop when buffer drains below HWM ([e24b4d3](https://github.com/Jota-project/jota-transcriber/commit/e24b4d31116f39a37ea95b058edacb2d1a451997))
* **tests:** remove protocol_version assertion from ValidConfigReturnsReady ([6249f50](https://github.com/Jota-project/jota-transcriber/commit/6249f504b9ea88ccc8856ac1c2ff7f35c9ce71d7))
* **tests:** resolve nlohmann_json and mosquitto when BUILD_SERVER=OFF ([fc143a0](https://github.com/Jota-project/jota-transcriber/commit/fc143a0af1453c64d7da57a40f8afd22c8e16bd7))
* update auth client, MQTT publisher, and Docker config ([b96e5db](https://github.com/Jota-project/jota-transcriber/commit/b96e5db85ce1b61d619091b67238c12bf5d66760))
* watchdog covers ioc_thread join, handle emplace_back exception without re-throw ([cbd24c9](https://github.com/Jota-project/jota-transcriber/commit/cbd24c93d4d38a745f6860c34dedeb3b731e8429))
* WebSocket handshake timeout bug ([90d516c](https://github.com/Jota-project/jota-transcriber/commit/90d516cad705a5cc8fd8c6dcf49ade23a5a6cdc1))


### Features

* add --auth-token for static local token authentication ([408e252](https://github.com/Jota-project/jota-transcriber/commit/408e2522fdbd41d3ac44969efd80828129366915))
* add ModelCache and refactor engine for shared context ([67d9924](https://github.com/Jota-project/jota-transcriber/commit/67d992404192d2444c51dbbfa2d8ff253335bbf1))
* add MQTT credentials and orchestrator integration guide ([c92169c](https://github.com/Jota-project/jota-transcriber/commit/c92169c40489cc4a18e4237f46a3c416580f1afe))
* add protocol_version to ready message + raw transcription fallback ([7189ff1](https://github.com/Jota-project/jota-transcriber/commit/7189ff1b79fc582d8c550bc4dfd0849cc8e8230d))
* add try_acquire() to InferenceLimiter for non-blocking inference ([aa60805](https://github.com/Jota-project/jota-transcriber/commit/aa608053774967a59e6989428da82d4335edac04))
* Añadir soporte Docker y GPU en Whisper ([dce466a](https://github.com/Jota-project/jota-transcriber/commit/dce466afadc11ef322962282fe34db7c8541c4df))
* async flush thread and write mutexes to fix dead silence ([62cadb4](https://github.com/Jota-project/jota-transcriber/commit/62cadb4387bb31aad62ed9c086ff5317cc97399a))
* audio buffer HWM (20s) with JSON warning to client on overflow ([24b91d8](https://github.com/Jota-project/jota-transcriber/commit/24b91d8d3b97927eec74fbd8edd9ec98b7d428ce))
* **auth:** add API-based authentication module ([f30b222](https://github.com/Jota-project/jota-transcriber/commit/f30b2221719b7250782056fc2453842bd5305883))
* **clients:** añadir cliente de pruebas en Python ([206b867](https://github.com/Jota-project/jota-transcriber/commit/206b867a45ef9f2154ff008bd317bf7812d6408d))
* **config:** add .env file loading and expanded CLI/env config ([5490bac](https://github.com/Jota-project/jota-transcriber/commit/5490bacb00631c5243c0e334b4c1478098a9dd38))
* Enterprise grade enhancements (DevOps/SRE) ([c2bdbd4](https://github.com/Jota-project/jota-transcriber/commit/c2bdbd488a7689c9509ffd7d017d23a90af8965c))
* handle idle timeouts, language caching, and telemetry endpoint ([7137dd5](https://github.com/Jota-project/jota-transcriber/commit/7137dd51eabdc0e6ff406d777a57388f9e4cb005))
* implement global inference limiter and audio preprocessing ([f4fc205](https://github.com/Jota-project/jota-transcriber/commit/f4fc205211f5d7ba7c86eb10f739cd074b916a39))
* implement sliding window and safe VAD for stable realtime performance ([d67f717](https://github.com/Jota-project/jota-transcriber/commit/d67f71739d225f80246c90e628494d713b0e8ba4))
* Implementar StreamingWhisperEngine y SimpleVAD con tests completos ([4f20a15](https://github.com/Jota-project/jota-transcriber/commit/4f20a157bc1045f3447d6bc7fab3430acf278756))
* integrate ModelCache into session/server + add config ([7183a1a](https://github.com/Jota-project/jota-transcriber/commit/7183a1a69757b0c7a17d684a5eee679765b18a23))
* **mqtt:** add MQTT publisher module ([483194f](https://github.com/Jota-project/jota-transcriber/commit/483194ff991358c72b3855aa39ffd81a178e877b))
* non-blocking inference in flushLoop using try_acquire ([89a032e](https://github.com/Jota-project/jota-transcriber/commit/89a032e3b894366c69def406c863c493a4a517bf))
* **perf:** first stable real-time transcription with hallucination guards ([2945547](https://github.com/Jota-project/jota-transcriber/commit/294554778b474940fd967c8ee8ac6af9cb1873a4))
* **perf:** semantic token sliding window and 250ms latency ([e23c624](https://github.com/Jota-project/jota-transcriber/commit/e23c6247a69590866b5b21066ebe063ae1ecec31))
* rename project to jota-transcriber ([bd85ba8](https://github.com/Jota-project/jota-transcriber/commit/bd85ba8cdddc7780b3f1f4c3f0671480d9608aa5))
* switch default model to ggml-small.bin for faster realtime performance ([835f6cd](https://github.com/Jota-project/jota-transcriber/commit/835f6cd9809e72f5ca451fcea2b3a7f116d44225))
* tune audio normalization, add whisper advanced parameters and logging ([74e04de](https://github.com/Jota-project/jota-transcriber/commit/74e04de6fcb2709ac80db6d2f3145609cfba1b6a))


### Performance Improvements

* Docker layer cache para whisper.cpp/CUDA ([#19](https://github.com/Jota-project/jota-transcriber/issues/19)) ([0afd7e8](https://github.com/Jota-project/jota-transcriber/commit/0afd7e84790bec845ac12b105bfa91913b9a13c7))

# Changelog

All notable changes to this project will be documented in this file.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
Versioning: [Semantic Versioning](https://semver.org/spec/v2.0.0.html)

---

## [Unreleased]

### Added
- `--auth-token` flag for static local token authentication — constant-time comparison, no external API required. Also available as `AUTH_TOKEN` env var.
- `protocol_version: 1` field in the `ready` message — clients can use this to detect protocol breaking changes in future releases.

### Fixed
- `buffer_overflowed_` flag now resets deterministically in `flushLoop` when inference drains the buffer below the 20s HWM, instead of waiting for the next client audio chunk.

### Docs
- `clients/README.md`: corrected the Protocol section — config message now shows the actual fields (`token`, `publish_mqtt`, `vad_thold`) and audio is documented as binary WebSocket frames (float32 LE), not JSON/base64.
- `clients/API_GUIDE.md`: `ready` message example now includes `beam_size`, `publish_mqtt`, and `protocol_version` fields that the server actually sends.

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
