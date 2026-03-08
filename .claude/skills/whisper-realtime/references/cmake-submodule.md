# CMake Submodule & Build Troubleshooting

## whisper.cpp as git submodule

### Setup

```bash
# .gitmodules (already configured):
[submodule "third_party/whisper.cpp"]
    path = third_party/whisper.cpp
    url = https://github.com/ggml-org/whisper.cpp.git

# After fresh clone:
git submodule update --init --recursive

# Update to latest whisper.cpp:
cd third_party/whisper.cpp && git pull origin master
cd ../.. && git add third_party/whisper.cpp && git commit -m "Update whisper.cpp"
```

### CMakeLists.txt integration (current — correct)

```cmake
# add_subdirectory exposes these targets:
#   whisper     — main library (links ggml, ggml-cpu, ggml-cuda if enabled)
#   ggml        — low-level tensor library
#   ggml-cuda   — CUDA backend (only if GGML_CUDA=1)

add_subdirectory(third_party/whisper.cpp)

# Your engine library links whisper (which transitively includes ggml):
target_link_libraries(streaming_whisper PUBLIC whisper)

# Includes: whisper.h is in third_party/whisper.cpp/include/
target_include_directories(streaming_whisper PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/whisper.cpp/include
)
```

## Build Configurations

### CPU only (development)
```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTS=ON \
    -DBUILD_SERVER=ON
cmake --build build -j$(nproc)
```

### CUDA (production)
```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_SERVER=ON \
    -DGGML_CUDA=1
cmake --build build -j$(nproc)
```

### macOS Metal (Apple Silicon)
```bash
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_METAL=1
cmake --build build -j$(sysctl -n hw.ncpu)
```

### OpenBLAS (CPU acceleration)
```bash
sudo apt-get install libopenblas-dev
cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_BLAS=ON \
    -DGGML_BLAS_VENDOR=OpenBLAS
cmake --build build -j$(nproc)
```

## Common Build Errors

### `whisper.h: No such file or directory`
```
fatal error: whisper.h: No such file or directory
```
**Cause**: Submodule not initialized or empty directory.
```bash
ls third_party/whisper.cpp/  # Should have CMakeLists.txt, include/, src/
git submodule update --init --recursive
```

### `undefined reference to 'whisper_init_from_file_with_params'`
**Cause**: Not linking against whisper target.
```cmake
# Wrong:
target_link_libraries(mylib ggml)
# Correct:
target_link_libraries(mylib whisper)
```

### `CUDA driver library not found` (Docker build)
```
nvcc fatal: Cannot find ptxas in PATH
# or
/usr/bin/ld: cannot find -lcuda
```
**Cause**: CUDA stubs not in library path during link.
```dockerfile
# In Dockerfile, before cmake:
ENV LIBRARY_PATH="/usr/local/cuda/lib64/stubs:${LIBRARY_PATH}"
# The stubs provide a libcuda.so stub for linking (not for runtime)
```

### `libggml-cuda.so: cannot open shared object file` (runtime)
**Cause**: Built with shared libs. Dynamic linker can't find .so.
```bash
# Fix: rebuild with static libs:
cmake -B build -DBUILD_SHARED_LIBS=OFF -DGGML_CUDA=1
# Verify binary is self-contained:
ldd build/transcription_server | grep "not found"  # should be empty
```

### `error: no matching function for call to 'whisper_full_default_params'`
**Cause**: API changed between whisper.cpp versions.
```bash
# Check your submodule version:
cd third_party/whisper.cpp && git log --oneline -5
# Check API in include/whisper.h for correct function signatures
```

### `FetchContent_MakeAvailable` timeout (nlohmann/json or gtest)
**Cause**: No internet access during build (Docker build context without network).
```cmake
# Option 1: Pre-cache with --no-deps in CI
# Option 2: Bundle the deps:
include_directories(third_party/nlohmann)  # header-only, just copy json.hpp
```

## Docker Build Checklist

```dockerfile
# Build stage must have:
RUN apt-get install -y \
    build-essential cmake git \
    libboost-all-dev libssl-dev \
    libmosquitto-dev libmosquittopp-dev

# COPY before cmake:
COPY . .  # includes third_party/whisper.cpp submodule

# Submodule must be populated before COPY
# In docker-compose or CI, ensure:
# git submodule update --init --recursive
# BEFORE docker build context is created

# Static build to avoid runtime .so issues:
RUN cmake -B build \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_CUDA=1 && \
    cmake --build build -j$(nproc)
```

## Verifying CUDA is Active

```bash
# Method 1: Check server startup log
./build/transcription_server --model models/ggml-base.bin 2>&1 | head -20
# Look for: "BLAS = 1" or "CUDA = 1" or "ggml_cuda_init: found X CUDA devices"

# Method 2: Check binary linkage
ldd build/transcription_server | grep -E "cuda|cublas"
# Should show: libcuda.so.1 => /usr/lib/x86_64-linux-gnu/libcuda.so.1

# Method 3: nvidia-smi during inference
watch -n 0.5 nvidia-smi
# GPU memory should increase when model loads (~500MB for base, ~2GB for medium)

# Method 4: Inference timing
# CPU base:   ~500ms per 10s audio
# GPU base:   ~30ms per 10s audio
# The difference is 15-20x — easy to spot
```

## Model Files

```bash
# Download using whisper.cpp's built-in script:
cd third_party/whisper.cpp
bash models/download-ggml-model.sh base    # 147MB
bash models/download-ggml-model.sh small   # 461MB
bash models/download-ggml-model.sh medium  # 1.5GB
bash models/download-ggml-model.sh large-v3 # 3GB

# Or manually from Hugging Face:
wget https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin

# Move to project models dir:
mv third_party/whisper.cpp/models/ggml-base.bin ./models/
```

## whisper.cpp API Stability Notes

whisper.cpp undergoes frequent breaking API changes. Key version-sensitive functions:

| Function | Notes |
|---|---|
| `whisper_context_default_params()` | Added in ~2023. Older versions use direct struct init. |
| `whisper_init_from_file_with_params()` | Replaces old `whisper_init_from_file()` |
| `whisper_full_with_state()` | Stable. Use this for thread-safe concurrent inference. |
| `flash_attn` in context params | Added ~late 2023. Not present in older versions. |
| `WHISPER_SAMPLING_GREEDY` enum | Replaces old numeric values in some versions. |

If build fails after updating submodule, check `include/whisper.h` for current API.