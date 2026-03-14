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
    libgomp1 \
    ca-certificates \
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