# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Real-time audio transcription microservice in C++17 using OpenAI Whisper (`whisper.cpp`). Clients stream raw PCM audio over WebSocket (WS or WSS), receiving partial and final transcription results as JSON.

## Build Commands

```bash
# Full build (server + tests)
cmake -B build -DBUILD_TESTS=ON -DBUILD_SERVER=ON -DBUILD_SHARED_LIBS=OFF
cmake --build build -j$(nproc)

# Server only (skip tests)
cmake -B build -DBUILD_TESTS=OFF -DBUILD_SERVER=ON -DBUILD_SHARED_LIBS=OFF
cmake --build build -j$(nproc)

# With CUDA/GPU support
cmake -B build -DBUILD_SHARED_LIBS=OFF -DGGML_CUDA=1
cmake --build build -j$(nproc)

# Run all tests
./run_tests.sh

# Run tests manually
./build/tests/test_streaming_whisper

# Run a single test
./build/tests/test_streaming_whisper --gtest_filter=StreamingWhisperTest.TranscribeSilence
```

Tests require a model file at `third_party/whisper.cpp/models/ggml-base.bin`.

## Running the Server

```bash
# Run Server:
./build/jota-transcriber --model /path/to/ggml-base.bin
# Or with params:
./build/jota-transcriber \
    --model /path/to/ggml-base.bin \
    --bind 0.0.0.0 --port 8003 \
    --whisper-beam-size 5 --whisper-threads 4 \
    --auth-token YOUR_TOKEN \
    --cert server.crt --key server.key \
    --max-connections 8 --max-connections-per-ip 2
```

# Generate self-signed certs
./generate_certs.sh

## Docker

```bash
# Build and start with GPU
docker-compose up --build

# Rebuild only
docker-compose build
```

Model files must be placed in `./models/` before starting (mounted to `/app/models/` in container). Default model: `ggml-base.bin`.

## Architecture

### Two-tier design

**Tier 1 — Transcription Engine** (`src/whisper/`)
- `StreamingWhisperEngine`: thread-safe wrapper around whisper.cpp C API
- Maintains circular PCM buffer (max 30 seconds @ 16kHz, float32)
- Exposes `addAudio(const int16_t*, size_t)`, `addAudioBytes(const char*, size_t)`, `transcribe()`, `reset()`

**Tier 2 — WebSocket Server** (`src/server/`)
- `server.cpp`: entry point, parses CLI, runs ASIO accept loop, spawns one thread per connection
- `StreamingSession<Stream>`: template class handling WebSocket framing and session lifecycle
  - Authenticates via JSON config message, then buffers incoming binary audio
  - Throttles: only calls `transcribe()` when ≥1 second of new audio has accumulated (prevents O(N²) reprocessing)
  - Sends `{"type":"transcription","text":"...","is_final":true/false}` responses
- `AuthManager`: constant-time token comparison to resist timing attacks
- `ConnectionLimiter` + `ConnectionGuard` (RAII): enforces global and per-IP connection caps
- `ServerConfig`: plain struct aggregating all runtime configuration

### WebSocket Protocol

1. Client connects (WS or WSS)
2. Client sends JSON config: `{"type":"config","language":"en","token":"TOKEN"}`
3. Client streams binary frames: raw PCM float32, 16kHz, mono
4. Server replies with partial transcriptions as audio accumulates
5. Client sends `{"type":"end"}` to signal end-of-stream → final transcription returned

### Linking — importante

whisper.cpp construye **librerías compartidas por defecto** (`BUILD_SHARED_LIBS=ON`). Esto hace que el binario quede enlazado contra `.so` con rutas absolutas del directorio de build, lo que rompe el despliegue en Docker y al mover el binario. Siempre compilar con **`-DBUILD_SHARED_LIBS=OFF`** para enlazar estáticamente ggml/whisper dentro del binario. Las dependencias de sistema que quedan en runtime son: `libssl3`, `libgomp1`, `libboost-system`, `libboost-thread`, `libstdc++6`.

### Key constraints
- `StreamingSession` is a **template** (`<Stream>`) to support both plain TCP (`tcp::socket`) and TLS (`ssl::stream<tcp::socket>`); implementation lives entirely in the `.h` file
- whisper.cpp is a **git submodule** at `third_party/whisper.cpp/`
- nlohmann/json and Google Test are fetched via CMake `FetchContent`
