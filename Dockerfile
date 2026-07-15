# Build Stage
#
# ⚠️  AVISO DE COMPATIBILIDAD CUDA — NO ACTUALIZAR A 12.3+ ⚠️
#
# Este Dockerfile usa CUDA 12.2 explícitamente. Es la última versión del toolkit
# NVIDIA compatible con GPUs de arquitectura Pascal (compute capability 6.1).
#
# GPUs afectadas: GTX 1060, GTX 1050 Ti, Tesla P4/P40/P100, Quadro Pxxxx, etc.
#
# A partir de CUDA 12.3, NVIDIA incluyó soporte para "forward compatibility" que
# requiere GPUs con compute capability ≥9.0 (Ampere o superior). Las GPUs Pascal
# no son compatibles y fallan con:
#
#   ggml_cuda_init: failed to initialize CUDA:
#   forward compatibility was attempted on non supported HW
#
# Más información:
#   - PR original: https://github.com/Jota-project/jota-transcriber/pull/63
#   - NVIDIA CUDA 12.2 Release Notes: compatible con Pascal
#   - NVIDIA CUDA 12.3+ Release Notes: requiere Ampere o superior
#
# AVISO: Dependabot está configurado para IGNORAR nvidia/cuda. Si necesitas
# actualizar por alguna razón, verifica primero la compatibilidad en:
#   https://github.com/Jota-project/jota-transcriber/pull/63
#
FROM nvidia/cuda:12.2.0-devel-ubuntu22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Instalamos dependencias de compilación
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libboost-all-dev \
    libssl-dev \
    pkg-config \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    libswresample-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Stubs de CUDA driver necesarios para linkar libggml-cuda
ENV LIBRARY_PATH="/usr/local/cuda/lib64/stubs:${LIBRARY_PATH}"

# ── Fase A: whisper.cpp + dependencias externas ────────────────────────────
# Esta capa solo se invalida cuando cambia third_party/ o CMakeLists.txt.
# Configura con BUILD_SERVER=ON para que FetchContent descargue nlohmann/json
# aquí (cmake no verifica existencia de .cpp al configurar, solo al compilar).
# Boost y OpenSSL ya están instalados arriba — los find_package pasan sin error.
COPY third_party/ third_party/
COPY CMakeLists.txt .
# CMake verifica existencia de fuentes en configure; crear stubs vacíos para
# que la configuración pase sin necesitar src/ todavía (se sobreescriben luego).
RUN mkdir -p src/whisper src/server src/server/handlers src/auth src/audio && \
    touch src/whisper/StreamingWhisperEngine.cpp \
          src/whisper/VadGate.cpp \
          src/server.cpp \
          src/server/StreamingSession.cpp \
          src/server/AuthManager.cpp \
          src/server/ConnectionLimiter.cpp \
          src/server/TrustedProxyResolver.cpp \
          src/server/ConnectionGuard.cpp \
          src/auth/ApiAuthClient.cpp \
          src/auth/AuthCache.cpp \
          src/audio/AudioDecoder.cpp \
          src/server/MultipartParser.cpp \
          src/server/HttpRouter.cpp \
          src/server/handlers/HandleHealth.cpp \
          src/server/handlers/HandleReady.cpp \
          src/server/handlers/HandleMetrics.cpp \
          src/server/handlers/HandleTranscribe.cpp
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SERVER=ON \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_CUDA=1 \
    -DCMAKE_CUDA_ARCHITECTURES=61

# Compila whisper + ggml (incluye kernels CUDA — la parte lenta).
RUN cmake --build build --target whisper -j2

# ── Fase B: código del servidor ────────────────────────────────────────────
# Solo se invalida cuando cambia src/ o generate_certs.sh.
# cmake detecta que whisper/ggml ya están compilados y los omite.
COPY src/ src/
COPY generate_certs.sh .
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SERVER=ON \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_CUDA=1
RUN cmake --build build --target jota-transcriber -j2

# Runtime Stage
# ⚠️  Mismo aviso de compatibilidad CUDA que en Build Stage (arriba).
# Usar siempre nvidia/cuda:12.2.0-runtime-ubuntu22.04 — NO actualizar.
FROM nvidia/cuda:12.2.0-runtime-ubuntu22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Librerías de ejecución esenciales
RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-system1.74.0 \
    libboost-thread1.74.0 \
    libssl3 \
    libgomp1 \
    ca-certificates \
    libavformat58 \
    libavcodec58 \
    libavutil56 \
    libswresample3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -ms /bin/bash appuser

WORKDIR /app

# Copia de binarios y utilidades
COPY --from=builder /app/build/jota-transcriber /app/jota-transcriber
COPY --from=builder /app/generate_certs.sh /app/generate_certs.sh

RUN mkdir -p /app/models && chown -R appuser:appuser /app
USER appuser

EXPOSE 8003

CMD ["./jota-transcriber"]
