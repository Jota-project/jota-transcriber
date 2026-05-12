# Análisis de Modelos — jota-transcriber

## 1. Modelos disponibles

### GGML (quantizados, CPU+GPU) — `download-ggml-model.sh`

| Modelo | Tamaño | VRAM aprox. | WER (es) | Velocidad | Streaming? |
|--------|--------|-------------|-----------|-----------|-----------|
| `tiny` | 39 MB | ~80 MB | ~25%+ | Muy rápida | ✅ |
| `tiny-q5_1` | 49 MB | ~100 MB | ~25%+ | Muy rápida | ✅ |
| `tiny-q8_0` | 64 MB | ~120 MB | ~24% | Muy rápida | ✅ |
| `base` | 142 MB | ~200 MB | ~15% | Rápida | ✅ |
| `base-q5_1` | 82 MB | ~150 MB | ~15% | Rápida | ✅ |
| `base-q8_0` | 138 MB | ~200 MB | ~14% | Rápida | ✅ |
| `small` | 466 MB | ~500 MB | ~10% | Buena | ✅ |
| `small-q5_1` | 290 MB | ~350 MB | ~9% | Buena | ✅ |
| `small-q8_0` | 464 MB | ~500 MB | ~8% | Buena | ✅ |
| `small.en-tdrz` | ? | | | | ✅ (inglés, tinydiarize) |
| `medium` | 1.5 GB | ~1.6 GB | ~7% | Aceptable | ⚠️ justa |
| `medium-q5_0` | 982 MB | ~1 GB | ~7% | Mejor que medium | ✅ |
| `medium-q8_0` | 1.4 GB | ~1.5 GB | ~6% | Aceptable | ⚠️ |
| `large-v1` | 2.9 GB | ~3 GB | ~5% | Lenta | ❌ |
| `large-v2-q5_0` | 1.8 GB | ~2 GB | ~5% | Lenta | ❌ |
| `large-v2-q8_0` | 2.7 GB | ~2.9 GB | ~4% | Lenta | ❌ |
| `large-v3` | 2.9 GB | ~3 GB | ~4% | Lenta | ❌ |
| `large-v3-q5_0` | 1.8 GB | ~2 GB | ~4% | Lenta | ❌ |
| `large-v3-turbo` | 1.6 GB | ~1.7 GB | ~5% | Media | ❌ |
| `large-v3-turbo-q5_0` | 1.0 GB | ~1.1 GB | ~5% | Media | ❌ |
| `large-v3-turbo-q8_0` | 1.5 GB | ~1.6 GB | ~4% | Media | ❌ |

> **`.en` = inglés solo** (no traduce otros idiomas, pero es más preciso en inglés).
> **`.tdrz` = tinydiarize** (especial para diálogos, solo inglés).
> **`-q5_0`, `-q5_1`, `-q8_0` = quantized** (menos VRAM, ligeramente menos preciso).

### VAD (Voice Activity Detection) — `download-vad-model.sh`

Separate from transcription models:

| Modelo | Tamaño | Uso |
|--------|--------|-----|
| `silero-v5.1.2` |很小| Detección de voz (silencios) |
| `silero-v6.2.0` |很小| Detección de voz (silencios) |

> El VAD ya está integrado en `StreamingWhisperEngine` opcionalmente via `setVadThreshold()`.

### CoreML (Apple Silicon) — `download-coreml-model.sh`

> ⚠️ **Script roto** ("hasn't been maintained and is not functional"). Ignorar.

### Lo que tienes ahora mismo en disco

```
models/
├── ggml-medium.bin    1.5 GB ← ACTUAL (default en .env)
└── ggml-small.bin      466 MB ← Descargado, pero NO es el default
```

---

## 2. Estado actual del código

### El campo `model` en HTTP está MUERTO

```cpp
// HandleTranscribe.cpp:100-102
if (!parts.count("model")) {
    sendError(http::status::bad_request, "Missing required field: model", ...);
    return;
}
```

Se **exige** que el cliente envíe el campo `model`, pero **nunca se usa**. Siempre se carga `config.model_path`:

```cpp
// HandleTranscribe.cpp:140-145
std::optional<ModelCache::Guard> model_guard;
try {
    model_guard.emplace(config.model_path);  // ← IGNORA el model del cliente
} catch (const std::exception& e) {
    ...
}
```

### En WebSocket es aún peor

`StreamingSession` recibe `model_path` en el **constructor**, que viene de `config.model_path`. La sesión recibe chunks de audio sin ninguna capacidad de cambiar de modelo a mitad de sesión.

### ModelCache: un solo slot

```cpp
// ModelCache.h: singleton con un solo modelo
if (ctx_ && loaded_path_ == model_path) {
    ++ref_count_;  // mismo modelo → reutilizar
    return ctx_;
}
// Diferente modelo → unload anterior → load nuevo (2-5 segundos)
```

---

## 3. Propuesta de diseño

### Arquitectura objetivo

```
HTTP /v1/audio/transcriptions
  └── Cliente envía campo "model"
  └── Servidor verifica si el modelo existe en disco
      ├── EXISTE → cargar y transcribir
      └── NO EXISTE → 400 con lista de modelos disponibles
  └── Usa MODEL_PATH como fallback por defecto

WebSocket ws://
  └── Configurado con MODEL_PATH_STREAMING (env)
  └── NO permite cambiar de modelo por sesión
  └── small o base para streaming fluido
```

### Plan paso a paso

#### Paso 1: Descubrimiento automático de modelos

Añadir una función que escanee el directorio `models/` y construya un mapa de modelos disponibles:

```cpp
// src/whisper/ModelRegistry.h (nuevo)
class ModelRegistry {
public:
    static ModelRegistry& instance();
    
    // Escanea models/ y construye la lista
    void scan(const std::string& models_dir);
    
    // ¿Existe este modelo?
    bool exists(const std::string& model_name) const;
    
    // Path completo a un modelo (sin extensión, buscar .bin)
    std::optional<std::string> resolve(const std::string& model_name) const;
    
    // Lista de modelos disponibles (para error messages)
    std::vector<std::string> available() const;
    
    // Modelo por defecto para streaming
    std::string defaultStreamingModel() const;  // ggml-small.bin
    
    // Modelo por defecto para batch
    std::string defaultBatchModel() const;     // ggml-medium.bin

private:
    std::unordered_map<std::string, std::string> models_; // name → path
};
```

Inicializar en `main()`:
```cpp
ModelRegistry::instance().scan("/app/models");
```

#### Paso 2: Dos variables de entorno

```bash
# .env
MODEL_PATH_BATCH=/app/models/ggml-medium.bin   # para HTTP
MODEL_PATH_STREAMING=/app/models/ggml-small.bin # para WebSocket
```

#### Paso 3: HTTP — activar el campo `model`

```cpp
// HandleTranscribe.cpp
std::string requested_model;
if (parts.count("model")) {
    requested_model.assign(parts.at("model").data.begin(),
                           parts.at("model").data.end());
} else {
    requested_model = "medium"; // default
}

// Verificar que existe
auto resolved = ModelRegistry::instance().resolve(requested_model);
if (!resolved) {
    auto available = ModelRegistry::instance().available();
    json err = {{"error", {{"message", "Model not found: " + requested_model},
                            {"available_models", available}}}};
    sendError(http::status::bad_request, err.dump(), ...);
    return;
}

// Usar el modelo resuelto
model_guard.emplace(*resolved);
```

#### Paso 4: WebSocket — fija modelo de streaming

```cpp
// StreamingSession — no cambia: usa config.model_path que apunta a streaming
// (streaming session vive en una sesión, no puede cambiar de modelo en mitad)
// Para cambiar de modelo, crear nueva sesión
```

#### Paso 5: Endpoint de información

```http
GET /v1/models
```

Devuelve:
```json
{
  "models": [
    {"name": "tiny", "size": "39M", "quality": "very_low", "vrama": "80MB"},
    {"name": "small", "size": "466M", "quality": "medium", "vrama": "500MB"},
    {"name": "medium", "size": "1.5G", "quality": "high", "vrama": "1.6GB"},
    {"name": "large-v3", "size": "2.9G", "quality": "very_high", "vrama": "3GB"}
  ],
  "default_streaming": "small",
  "default_batch": "medium"
}
```

---

## 4. Routing recomendado por tipo de cliente

| Cliente | Modelo recomendado | Razón |
|---------|------------------|-------|
| Web (streaming en vivo) | `small` | Rápido, errores aceptables |
| App móvil | `small` o `base` | Limitación de CPU/memoria |
| Transcripción de archivos (API) | `medium` o `medium-q5_0` | Máxima calidad, sin presión de tiempo |
| Fallback/debug | `tiny` | Muy rápido para probar |

---

## 5. Gestión de modelos ausentes

### Si el usuario pide un modelo que no existe:

```cpp
// HTTP — error claro con ayuda
400 Bad Request
{
  "error": {
    "message": "Model 'large-v3' not found",
    "available_models": ["tiny", "base", "small", "medium", "large-v3-turbo"],
    "hint": "Download models using: ./download-ggml-model.sh <name> models/"
  }
}
```

### Si el usuario pide un modelo que no cabe en VRAM:

```cpp
// Intentar cargar → catch OOM → error con explicación
500 Internal Server Error
{
  "error": {
    "message": "Model too large for available GPU memory",
    "model": "large-v3 (2.9 GB)",
    "available_vram": "3 GB (of which ~1.5 GB free)",
    "hint": "Try medium-q5_0 (982 MB) instead"
  }
}
```

---

## 6. Modelo por defecto

El default actual en `ServerConfig.h` es:
```cpp
std::string model_path = "third_party/whisper.cpp/models/ggml-small.bin";
```

Pero en `.env` se sobreescribe a:
```bash
MODEL_PATH=/app/models/ggml-medium.bin
```

Y en StreamingSession se usa siempre `config.model_path` (que es el mismo `.env`).

---

## 7. Próximos modelos a descargar (recomendación)

```bash
# En worker-01, dentro del container o en el host
cd /home/sito/jota-transcriber/models

# Medium quantizado — misma calidad que medium, menos VRAM
./download-ggml-model.sh medium-q5_0 .

# Small con quantización — mejor que small normal
./download-ggml-model.sh small-q5_1 .

# Turbo — buena calidad, menor que medium
./download-ggml-model.sh large-v3-turbo .
```

---

## 8. Resumen de cambios necesarios

| Cambio | Prioridad | Riesgo |
|--------|-----------|--------|
| `ModelRegistry` — escaneo automático de `models/` | Alta | Bajo |
| `MODEL_PATH_STREAMING` + `MODEL_PATH_BATCH` en .env | Alta | Bajo |
| Implementar campo `model` en `HandleTranscribe` | Alta | Medio |
| Endpoint `GET /v1/models` | Media | Bajo |
| `ModelCache` — aceptar path explícito (ya lo hace) | ✅ Ya funciona | — |
| WebSocket — no cambiar modelo por sesión | ✅ Diseño correcto | — |
| Descargar `medium-q5_0` y `small-q5_1` | Media | Nulo |

---

## 9. Glosario

- **GGML**: Formato de modelo quantizado de whisper.cpp. Pesos comprimidos, más pequeño y rápido con ligera pérdida de precisión.
- **Quantization (-q5_0, -q8_0)**: Técnicas de compresión con distintos niveles. Q8 = mayor precisión, Q5 = balance, más bajo = más compresión.
- **WER (Word Error Rate)**: % de palabras maltranscritas. Más bajo = mejor.
- **Streaming**: Audio llega en chunks ~250ms, se transcribe en tiempo real.
- **Batch/HTTP**: Cliente sube archivo completo, recibe transcripción entera.
- **Beam search**: Decodificación más precisa que greedy. Más lenta.
- **tdrz (tinydiarize)**: Modelo especial para conversaciones/multiparlante (inglés).
- **VAD (Voice Activity Detection)**: Detecta cuándo hay voz vs silencio.
- **CoreML**: Formato Apple Silicon. Script roto, ignorar.
