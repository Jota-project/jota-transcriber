![Status: Maintained](https://img.shields.io/badge/status-Maintained-2ea44f)

> **Role in Jota ecosystem:** STT streaming microservice for the Jota gateway. Receives PCM Float32 audio over WebSocket and emits partial + final transcriptions. Receives `language` and `vad_thold` from the gateway's per-client `ClientConfig`.
>
> Part of [Jota-project](https://github.com/Jota-project). See [`ARCHITECTURE.md`](https://github.com/Jota-project/.github/blob/main/ARCHITECTURE.md) for the full system map.

---

# Real-Time C++ Transcription Microservice

[![CI State](https://github.com/jota-project/jota-transcriber/actions/workflows/ci.yml/badge.svg)](https://github.com/jota-project/jota-transcriber/actions)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CUDA Support](https://img.shields.io/badge/CUDA-Ready-green.svg)](https://developer.nvidia.com/cuda-zone)

High-performance real-time audio transcription microservice built with C++17 and [whisper.cpp](https://github.com/ggerganov/whisper.cpp). Clients stream raw PCM audio over WebSocket and receive partial and final transcription results as JSON.

## Features

- Real-time streaming with partial transcriptions while audio is being sent
- WebSocket (WS) and WebSocket over TLS (WSS)
- Authentication: static token or external API with in-memory cache
- Per-IP and global connection limits, with an opt-in DNS-based exemption for a trusted gateway proxy
- Non-blocking inference — GPU saturation skips a cycle instead of blocking
- Audio buffer high-water mark (20s) with `flow_control` pause/resume and `dropped_chunks` warnings
- Hallucination guard against Whisper decoder loops
- Prometheus metrics at `/metrics`, health check at `/health`, readiness at `/ready`
- Docker with NVIDIA GPU support

## Quick Start

### Prerequisites

- CMake 3.16+, GCC 9+ or Clang 10+ (C++17)
- Boost (asio, beast, system, thread)
- OpenSSL

### Build

```bash
# Clone with submodules (whisper.cpp is at third_party/whisper.cpp/)
git clone --recursive https://github.com/your-username/transcription-service.git
cd transcription-service

# Build server + tests (static linking)
cmake -B build -DBUILD_TESTS=ON -DBUILD_SERVER=ON -DBUILD_SHARED_LIBS=OFF
cmake --build build -j$(nproc)
```

### Download a model

```bash
# Example: small model (~500 MB, good balance of speed and accuracy)
cd third_party/whisper.cpp/models
./download-ggml-model.sh small

# Silero VAD model (required — silence gating is always on)
./download-vad-model.sh silero-v5.1.2
```

### Run

```bash
# Minimal — no auth, no TLS
./build/jota-transcriber --model third_party/whisper.cpp/models/ggml-small.bin

# With static auth token and TLS
./build/jota-transcriber \
  --model /path/to/ggml-small.bin \
  --bind 0.0.0.0 --port 9001 \
  --auth-token YOUR_TOKEN \
  --cert server.crt --key server.key \
  --max-connections 10 \
  --max-connections-per-ip 2 \
  --whisper-beam-size 5 \
  --whisper-threads 4

# With external auth API (validates tokens against your own backend)
./build/jota-transcriber \
  --model /path/to/ggml-small.bin \
  --auth-api-url http://auth-service:8080/validate \
  --auth-api-secret API_SECRET

# With a trusted gateway proxy (exempt from the per-IP cap — see below)
./build/jota-transcriber \
  --model /path/to/ggml-small.bin \
  --max-connections 8 --max-connections-per-ip 2 \
  --trusted-proxy-hosts jota-gateway --trusted-proxy-refresh-sec 30

# Generate self-signed certs for development
./generate_certs.sh
```

> **Note on auth:** `AUTH_API_URL` was originally designed to point to `jota-db`. The recommended path forward is per-service `AUTH_TOKEN` (static), with `AUTH_API_URL` kept only for setups that still centralize auth via `jota-db`. The deprecation of `jota-db` as default auth backend is tracked in [`Jota-project/jota-gateway` issues](https://github.com/Jota-project/jota-gateway/issues). Leaving **both** `AUTH_TOKEN` and `AUTH_API_URL` empty disables authentication entirely — every token is accepted. Only rely on this when network placement already restricts who can reach the port (e.g. `network_mode: host` with no public bind, or a firewalled internal network).

### Run tests

```bash
./run_tests.sh
# or directly: ./build/tests/unit_tests
```

Tests that require a model file skip automatically if `third_party/whisper.cpp/models/ggml-small.bin` is absent.

## Docker

```bash
# Build and start (requires an NVIDIA GPU — this Dockerfile always builds
# with CUDA baked in, there is no CPU-only build target)
docker-compose up --build
```

Model files must be placed in `./models/` before starting (mounted at `/app/models/` inside the container). This must include the Silero VAD model (`ggml-silero-v5.1.2.bin`) alongside the main Whisper model — silence gating is always on and has no environment-variable override, so `docker-compose.yml` passes `--vad-model /app/models/ggml-silero-v5.1.2.bin` explicitly.

## CLI Reference

| Flag | Default | Description |
|---|---|---|
| `--model PATH` | `ggml-small.bin` | Path to Whisper GGML model file |
| `--bind ADDR` | `0.0.0.0` | Bind address |
| `--port N` | `9001` | TCP port |
| `--cert FILE` | — | TLS certificate (enables WSS) |
| `--key FILE` | — | TLS private key |
| `--auth-token TOKEN` | — | Static token — constant-time comparison, no external API needed |
| `--auth-api-url URL` | — | External auth API (takes precedence over `--auth-token`) |
| `--auth-api-secret SECRET` | — | Bearer secret for the auth API |
| `--auth-cache-ttl N` | `300` | Auth result cache TTL in seconds |
| `--auth-api-timeout N` | `5` | Auth API request timeout in seconds |
| `--max-connections N` | `8` | Global connection cap |
| `--max-connections-per-ip N` | `2` | Per-IP connection cap |
| `--trusted-proxy-hosts HOSTS` | — | CSV hostnames exempt from the per-IP cap (empty = disabled). See [Trusted Proxy Exemption](#trusted-proxy-exemption-per-ip-limit) |
| `--trusted-proxy-refresh-sec N` | `30` | DNS re-resolution interval for `--trusted-proxy-hosts` |
| `--session-timeout-sec N` | `30` | Idle session timeout |
| `--shutdown-timeout-sec N` | `10` | Graceful shutdown wait |
| `--whisper-beam-size N` | `1` | Beam size (1 = greedy, fastest) |
| `--whisper-threads N` | `4` | CPU threads per inference |
| `--max-concurrent-inference N` | `4` | Max simultaneous Whisper decodes |
| `--model-cache-ttl N` | `300` | Seconds to keep model loaded after last session (-1 = forever) |
| `--whisper-initial-prompt TEXT` | — | Decoder initial prompt for vocabulary guidance |
| `--whisper-temperature F` | `0.0` | Initial sampling temperature (`0.0` = greedy, fastest for streaming) |
| `--whisper-temperature-inc F` | `0.0` | Temperature increment on repetition fallback (`0.0` disables fallback) |
| `--whisper-no-speech-thold F` | `0.3` | Probability threshold to reject non-speech segments |
| `--whisper-logprob-thold F` | `-0.7` | Log-prob threshold to reject low-confidence segments (`-1.0` disables) |
| `--flush-min-new-audio-ms N` | `500` | ms of new audio required before `flushLoop` re-runs inference |
| `--max-upload-bytes N` | `26214400` (25 MB) | Max body size for `POST /v1/audio/transcriptions` |
| `--vad-model PATH` | `third_party/whisper.cpp/models/ggml-silero-v5.1.2.bin` | Silero VAD model file — silence gating is always on |
| `--vad-threshold F` | `0.5` | VAD speech probability threshold |
| `--vad-min-speech-ms N` | `250` | Minimum speech segment duration |
| `--vad-min-silence-ms N` | `2000` | Silence ≥ this duration is trimmed before Whisper decoding |
| `--vad-max-speech-s F` | unlimited | Max speech segment duration before a forced split |
| `--vad-speech-pad-ms N` | `400` | Audio kept on each side of a detected speech segment |
| `--vad-samples-overlap F` | `0.1` | Overlap (seconds) between adjacent speech segments |

All flags are also available as environment variables (see `.env.example`), **except the `--vad-*` flags**, which are CLI-only — pass them via `command:` in `docker-compose.yml` if not using the defaults.

## Trusted Proxy Exemption (Per-IP Limit)

`--max-connections-per-ip` caps how many simultaneous sessions a single IP can hold, to stop one client from monopolizing the service. In production the only real client is often a single shared gateway process (e.g. `jota-gateway`) proxying many real users from one IP — which turns the per-IP cap into a de-facto global cap, independent of how many real users are active.

`--trusted-proxy-hosts` exempts a configured gateway (identified by hostname, resolved via DNS) from the per-IP cap. This **only** widens the per-IP allowance — the global `--max-connections` cap and authentication (`--auth-token` / `--auth-api-url`) are never affected.

**How it works:**
- Configure one or more hostnames (not IPs), comma-separated. Empty (the default) = feature off, zero behavior change.
- Every `--trusted-proxy-refresh-sec` seconds (default `30`), the server re-resolves each hostname via `getaddrinfo` (IPv4 + IPv6) and caches the result.
- **Fail-closed:** a hostname that has never resolved trusts nothing. A later transient resolution failure keeps that host's last known-good IPs — one flaky host among several doesn't undo another host's trust.
- Internals: `src/server/TrustedProxyResolver.h` / `.cpp`.

**Configuration:**

| CLI flag | Env var | Default | Description |
|---|---|---|---|
| `--trusted-proxy-hosts HOSTS` | `TRUSTED_PROXY_HOSTS` | *(empty = disabled)* | Comma-separated hostnames exempt from the per-IP cap |
| `--trusted-proxy-refresh-sec N` | `TRUSTED_PROXY_REFRESH_SEC` | `30` | DNS re-resolution interval in seconds |

**Deploy scenarios:**
- **docker-compose / same host** — this repo's `docker-compose.yml` uses `network_mode: host`, so the gateway and transcriber share the host's network namespace; `--trusted-proxy-hosts localhost` (or the host's own hostname) is usually correct.
- **Kubernetes** — point at the gateway's Service DNS name, e.g. `--trusted-proxy-hosts jota-gateway.default.svc.cluster.local`. CoreDNS resolves it to the current pod IP(s) on each refresh, so pod restarts/rescheduling don't require a config change.
- **Dual-stack caveat** — on a dual-stack bind (`--bind ::`), Boost.Asio reports IPv4 peers as `::ffff:a.b.c.d`, but DNS resolution returns bare IPv4 addresses, so an IPv4 trusted host won't match in that setup. This fails closed (not a security issue), but is a silent no-op worth knowing about when troubleshooting.

## WebSocket Protocol

Full protocol documentation in [`clients/API_GUIDE.md`](clients/API_GUIDE.md).

### Session flow

```
Client                           Server
  │                                 │
  │──── WebSocket connect ─────────►│
  │──── JSON: config ──────────────►│
  │◄─── JSON: ready ────────────────│
  │                                 │
  │──── Binary: PCM float32 ───────►│
  │──── Binary: PCM float32 ───────►│
  │◄─── JSON: transcription (partial)│
  │──── Binary: PCM float32 ───────►│
  │◄─── JSON: transcription (partial)│
  │                                 │
  │──── JSON: end ─────────────────►│
  │◄─── JSON: transcription (final) │
  │◄─── WebSocket close ────────────│
```

### Config message (client → server)

```json
{
  "type": "config",
  "language": "es",
  "token": "YOUR_TOKEN"
}
```

- `language`: ISO 639-1 code (`"es"`, `"en"`, `"fr"`, …) or `"auto"` for detection. Default: `"es"`.
- `token`: required only if the server has auth enabled.
- `vad_thold`: VAD threshold `[0.0–1.0]`. `0.0` disables VAD. Default: `0.0`.

### Audio format

Binary WebSocket frames — raw PCM float32, 16 kHz, mono, little-endian. Recommended chunk size: 100–500 ms.

```python
# Convert int16 PCM to float32
float_samples = int16_samples.astype(np.float32) / 32768.0
ws.send(float_samples.tobytes())
```

### Server messages

| `type` | When |
|---|---|
| `ready` | Session configured successfully |
| `transcription` | Partial (`is_final: false`) or final (`is_final: true`) result |
| `warning` | Non-fatal issue. `code: "buffer_full"` when the 20s buffer is saturated — sent every 10 dropped chunks while it stays saturated (field `dropped_chunks`, resets to 0 each time the buffer drains and a new saturation episode starts) |
| `flow_control` | `action: "pause"` when the buffer hits the 20s high-water mark and the server starts dropping incoming audio; `action: "resume"` once it drains back below the 10s low-water mark. Informational — the server keeps dropping audio at the high-water mark regardless of whether the client honors this signal |
| `error` | Fatal session error — connection closes after `AUTH_FAILED`, `AUTH_REQUIRED` |

### End of stream

```json
{"type": "end"}
```

The server flushes the buffer, returns a final `transcription`, and closes the connection.

## HTTP Endpoints

The same port serves both WebSocket and HTTP:

| Endpoint | Description |
|---|---|
| `GET /health` | Returns `{"status": "ok"}` — always 200 if the process is alive |
| `GET /ready` | Returns `{"status": "ready"}` (200) or `{"status": "busy"}` (503) based on inference capacity |
| `GET /metrics` | Prometheus text format — active inferences, connections, model load state |

## Architecture

### Two-tier design

**Tier 1 — Transcription engine** (`src/whisper/`)
- `StreamingWhisperEngine`: thread-safe wrapper around `whisper_full_with_state()`
- Sliding window with semantic segment commit — partials flow continuously, committed text is never re-sent
- `ModelCache`: singleton with reference counting and TTL unload
- `InferenceLimiter`: semaphore with blocking `acquire()` and non-blocking `try_acquire()`

**Tier 2 — WebSocket server** (`src/server/`)
- `StreamingSession<Stream>`: template over plain TCP / TLS stream, handles framing and session lifecycle
- `flushLoop`: dedicated thread per session — decoupled from receive loop, uses `try_acquire()` to skip when GPU is busy
- `ConnectionLimiter` + `ConnectionGuard`: RAII global and per-IP caps
- `SessionTracker`: enables graceful shutdown of all active sessions on SIGINT/SIGTERM

### Key build constraint

Always build with `-DBUILD_SHARED_LIBS=OFF`. whisper.cpp defaults to shared libs, which breaks deployment when the binary is moved or run in Docker.

## Testing

```bash
./run_tests.sh                                      # all 144 tests
./build/tests/unit_tests --gtest_filter=AudioPipeline*      # one suite
./build/tests/unit_tests --gtest_filter=-*ModelCache*       # skip model-dependent
```

| Test file | Tests | Needs model |
|---|---|---|
| `test_hallucination_guard.cpp` | 10 | No |
| `test_audio_pipeline.cpp` | 8 | No |
| `test_inference_limiter.cpp` | 14 | No |
| `test_connection_limiter.cpp` | 10 | No |
| `test_trusted_proxy_resolver.cpp` | 12 | No |
| `test_session_tracker.cpp` | 4 | No |
| `test_model_cache.cpp` | 10 | Yes |
| `test_streaming_whisper_engine.cpp` | 25 | Yes |

## Client Examples

See [`clients/`](clients/) for a Python test client (file / mic / synthetic audio) and the full API reference.

## Status & roadmap

- **Status:** Maintained. Recent work (2026-05) focused on stability fixes — `ModelCache` RAII, `AudioDecoder` robustness, `InferenceLimiter` TOCTOU.
- **Known issue:** race condition between `flushLoop` and `handleEnd` may emit duplicate `is_final` transcriptions. Fix is straightforward; see issue #27.
- **Auth migration:** the default auth path will move from `jota-db` external API to per-service `AUTH_TOKEN` static. `AUTH_API_URL` will remain as an option.

---

*Built with [whisper.cpp](https://github.com/ggerganov/whisper.cpp), Boost.Beast, and Boost.Asio.*
