# AGENTS.md — jota-transcriber

## Build

```bash
# Always static linking (required for Docker deployment)
cmake -B build -DBUILD_SHARED_LIBS=OFF

# Server + tests
cmake -B build -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=ON -DBUILD_SERVER=ON
cmake --build build -j$(nproc)

# Tests only (used by CI)
cmake -B build -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=ON -DBUILD_SERVER=OFF
cmake --build build --target unit_tests -j$(nproc)
```

CUDA: add `-DGGML_CUDA=1`. macOS Metal: add `-DGGML_METAL=1`.

## Running — CRITICAL

**Production = Docker only.** Never leave the binary running as a standalone process.

Compiling and running the binary directly (via CMake) is acceptable **only for quick testing/debugging**. After testing, always kill the process.

```bash
# Quick test (compile + run)
cmake -B build -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=ON -DBUILD_SERVER=ON
cmake --build build -j$(nproc)
./build/jota-transcriber --model third_party/whisper.cpp/models/ggml-base.bin
# ...do your test...
pkill -f jota-transcriber  # ALWAYS kill after direct binary execution
```

Production start:
```bash
docker-compose up --build
```
Model files go in `./models/` (mounted at `/app/models/` in container).

## Tests

```bash
./run_tests.sh                              # all tests (compiles tests first)
./build/tests/unit_tests --gtest_filter=AudioPipeline*   # single suite
./build/tests/unit_tests --gtest_filter=-*ModelCache*   # exclude model-dependent
```

Test binary name is `unit_tests`, not `test_streaming_whisper` (CLAUDE.md has stale name).
Model-dependent tests skip automatically when model is absent.

## Submodule

`whisper.cpp` is a git submodule at `third_party/whisper.cpp/`. After fresh clone:
```bash
git submodule update --init --recursive
```

## Key Constraints

- **Always `-DBUILD_SHARED_LIBS=OFF`** — shared libs break Docker deployment (binary linked against `.so` with absolute paths from build dir)
- **Dockerfile exposes port 8003** — not 9001 (stale value in CLAUDE.md)
- `StreamingSession<Stream>` is a template in `.h` file, supports both plain TCP and TLS
- nlohmann/json and Google Test are fetched via CMake `FetchContent` automatically — no manual install needed

## Architecture

- **Tier 1** (`src/whisper/`): `StreamingWhisperEngine` — thread-safe wrapper around `whisper_full_with_state()`, sliding window, `ModelCache` singleton, `InferenceLimiter` semaphore
- **Tier 2** (`src/server/`): `StreamingSession<Stream>` handles WS framing; `flushLoop` thread per session uses non-blocking `try_acquire()` to skip GPU-saturated cycles

## Relevant Files

- `CLAUDE.md` — detailed project overview and architecture (read this first)
- `.claude/skills/whisper-realtime/SKILL.md` — load this skill when working with whisper.cpp, audio buffering, sliding window, or inference tuning
- `.claude/skills/whisper-realtime/references/cmake-submodule.md` — CMake/submodule troubleshooting, CUDA verification, model download
- `.env.example` — all CLI flags as env vars
- `.github/workflows/ci.yml` — exact CI build/test pipeline