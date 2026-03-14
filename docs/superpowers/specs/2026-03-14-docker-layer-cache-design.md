# Docker Layer Cache para whisper.cpp/CUDA — Design

## Problema

El `Dockerfile` actual hace un `COPY . .` antes de compilar, lo que invalida todas las capas
Docker cada vez que cambia cualquier archivo (incluyendo `src/`). Esto fuerza recompilar
whisper.cpp y los kernels CUDA completos en cada build, aunque no hayan cambiado.

## Objetivo

Conseguir que `docker-compose build` (y un eventual job de CI) solo recompile el código de
`src/` cuando no ha cambiado el submodule de whisper.cpp, eliminando el coste de compilación
CUDA/whisper en el flujo habitual de desarrollo.

## Solución: Separación de capas en el Dockerfile

Dividir el builder stage en dos fases con `COPY` separados:

### Fase A — whisper.cpp + dependencias externas (cacheada, lenta, rara)

```dockerfile
COPY third_party/ third_party/
COPY CMakeLists.txt .
# Configura con BUILD_SERVER=ON para que FetchContent descargue nlohmann/json ahora.
# cmake no verifica existencia de .cpp al configurar (solo al compilar), así que
# funciona aunque src/ aún no esté copiado.
# Boost y OpenSSL ya están instalados por el apt-get anterior — los find_package pasan.
# Esta capa queda cacheada con nlohmann/json ya descargado.
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SERVER=ON \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_CUDA=1
# Compila solo la librería whisper (y ggml) — los kernels CUDA pesados.
RUN cmake --build build --target whisper -j$(nproc)
```

Esta capa **solo se invalida** cuando cambia `third_party/` (el submodule de whisper.cpp)
o `CMakeLists.txt`. Ambos casos son poco frecuentes. `nlohmann/json` también queda
pre-descargado en esta capa, evitando una llamada de red en Fase B.

### Fase B — código del servidor (rápida, frecuente)

```dockerfile
COPY src/ src/
COPY generate_certs.sh .
# Reconfigura: cmake detecta que la configuración no ha cambiado y no recompila
# los objetos de whisper ya existentes en el layer anterior.
RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SERVER=ON \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_CUDA=1
# Compila StreamingWhisperEngine.cpp (src/whisper/) y todos los archivos de src/server/,
# src/auth/, src/log/, src/utils/ y el ejecutable jota-transcriber.
# whisper y ggml ya están compilados — cmake los omite.
RUN cmake --build build --target jota-transcriber -j$(nproc)
```

cmake detecta que los objetos de `whisper` y `ggml` están al día y no los recompila.
Lo que se compila en Fase B: `streaming_whisper` (wrapper en `src/whisper/`) y todo
el código del servidor en `src/`.

### Runtime stage

Sin cambios respecto al actual.

## CI: job de Docker build con caché de capas

Añadir un job `docker-build` al workflow de CI (`.github/workflows/ci.yml`) que:

1. Use `docker/setup-buildx-action` (habilita BuildKit)
2. Use `docker/build-push-action` con:
   - `push: false` (solo build, no push)
   - `cache-from: type=gha` — lee capas cacheadas de GitHub Actions cache
   - `cache-to: type=gha,mode=max` — escribe todas las capas (incluyendo intermedias)
3. Se ejecute en `push` a `main` y en pull requests

Con esto, la capa de whisper.cpp compilada queda persistida en el cache de GHA (~5 GB
disponibles gratis) y los runs siguientes la reutilizan directamente. Nota: los artefactos
CUDA de whisper pueden ser voluminosos; si se supera el límite de caché de GHA, la capa
se evicta y se recompila en el siguiente run (comportamiento degradado, no rotura).

## Flujo resultante

| Escenario | Qué se recompila | Tiempo esperado |
|-----------|-----------------|-----------------|
| Cambia `src/` (incluyendo headers) | `streaming_whisper` + código del servidor | Rápido (segundos/minuto) |
| Cambia `CMakeLists.txt` | whisper.cpp + servidor | Lento (igual que ahora) |
| Cambia submodule whisper.cpp | whisper.cpp + servidor | Lento (igual que ahora) |

## Archivos modificados

- `Dockerfile` — reestructurar el builder stage
- `.github/workflows/ci.yml` — añadir job `docker-build`

## Archivos no modificados

- `docker-compose.yml` — sin cambios
- `CMakeLists.txt` — sin cambios
- Runtime stage del Dockerfile — sin cambios
