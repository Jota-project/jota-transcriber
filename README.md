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
- Per-IP and global connection limits
- Non-blocking inference — GPU saturation skips a cycle instead of blocking
- Audio buffer high-water mark (20s) with client-side warning
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

# Generate self-signed certs for development
./generate_certs.sh
```

### Run tests

```bash
./run_tests.sh
# or directly: ./build/tests/unit_tests
```

Tests that require a model file skip automatically if `third_party/whisper.cpp/models/ggml-small.bin` is absent.

## Docker

```bash
# Build and start with GPU
docker-compose up --build

# CPU only
docker-compose build --build-arg GGML_CUDA=0
docker-compose up
```

Model files must be placed in `./models/` before starting (mounted at `/app/models/` inside the container).

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
| `--session-timeout-sec N` | `30` | Idle session timeout |
| `--shutdown-timeout-sec N` | `10` | Graceful shutdown wait |
| `--whisper-beam-size N` | `1` | Beam size (1 = greedy, fastest) |
| `--whisper-threads N` | `4` | CPU threads per inference |
| `--max-concurrent-inference N` | `4` | Max simultaneous Whisper decodes |
| `--model-cache-ttl N` | `300` | Seconds to keep model loaded after last session (-1 = forever) |
| `--whisper-initial-prompt TEXT` | — | Decoder initial prompt for vocabulary guidance |

All flags are also available as environment variables (see `.env.example`).

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
| `warning` | Non-fatal issue (e.g. `code: "buffer_full"` when the 20s buffer is saturated) |
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
./run_tests.sh                                      # all 68 tests
./build/tests/unit_tests --gtest_filter=AudioPipeline*      # one suite
./build/tests/unit_tests --gtest_filter=-*ModelCache*       # skip model-dependent
```

| Test file | Tests | Needs model |
|---|---|---|
| `test_hallucination_guard.cpp` | 9 | No |
| `test_audio_pipeline.cpp` | 8 | No |
| `test_inference_limiter.cpp` | 8 | No |
| `test_connection_limiter.cpp` | 7 | No |
| `test_session_tracker.cpp` | 4 | No |
| `test_model_cache.cpp` | 7 | Yes |
| `test_streaming_whisper_engine.cpp` | 25 | Yes |

## Client Examples

See [`clients/`](clients/) for a Python test client (file / mic / synthetic audio) and the full API reference.

---

*Built with [whisper.cpp](https://github.com/ggerganov/whisper.cpp), Boost.Beast, and Boost.Asio.*
