# Build Stage
FROM nvidia/cuda:13.2.1-devel-ubuntu22.04 AS builder

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
          src/server.cpp \
          src/server/StreamingSession.cpp \
          src/server/AuthManager.cpp \
          src/server/ConnectionLimiter.cpp \
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
    -DGGML_CUDA=1

# Compila whisper + ggml (incluye kernels CUDA — la parte lenta).
RUN cmake --build build --target whisper -j$(nproc)

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
RUN cmake --build build --target jota-transcriber -j$(nproc)

# Runtime Stage
FROM nvidia/cuda:13.2.1-runtime-ubuntu22.04 AS runtime

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
