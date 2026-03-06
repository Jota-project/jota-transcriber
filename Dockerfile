# Build Stage
FROM nvidia/cuda:12.3.1-devel-ubuntu22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Instalamos dependencias de compilación
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libboost-all-dev \
    libssl-dev \
    libmosquitto-dev \
    libmosquittopp-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Compilación con soporte CUDA
# Stubs de CUDA driver necesarios para linkar libggml-cuda.so
ENV LIBRARY_PATH="/usr/local/cuda/lib64/stubs:${LIBRARY_PATH}"
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DBUILD_SERVER=ON \
    -DGGML_CUDA=1 \
    -DBUILD_SHARED_LIBS=OFF && \
    cmake --build build -j$(nproc)

# Runtime Stage
FROM nvidia/cuda:12.3.1-runtime-ubuntu22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Librerías de ejecución esenciales
RUN apt-get update && apt-get install -y --no-install-recommends \
    libboost-system1.74.0 \
    libboost-thread1.74.0 \
    libssl3 \
    libmosquitto1 \
    libmosquittopp1 \
    libgomp1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copia de binarios y utilidades
COPY --from=builder /app/build/transcription_server /app/transcription_server
COPY --from=builder /app/generate_certs.sh /app/generate_certs.sh

RUN mkdir -p /app/models
EXPOSE 8003

CMD ["./transcription_server"]