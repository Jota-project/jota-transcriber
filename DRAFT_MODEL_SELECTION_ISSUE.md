# [FEATURE] Soporte para selección de modelo por petición HTTP

## Contexto

El servidor jota-transcriber actualmente carga **un solo modelo** configurado en `MODEL_PATH` (`.env`). El campo `model` enviado en las peticiones HTTP a `/v1/audio/transcriptions` es **exigido pero completamente ignorado**, y WebSocket streaming usa exclusivamente `config.model_path` sin posibilidad de cambio.

## Problema

1. **El campo `model` en HTTP no funciona**: el código lo pide pero nunca lo usa — siempre carga `config.model_path`
2. **No hay forma de descubrir modelos**: el usuario no puede saber qué modelos están disponibles en el servidor
3. **Un solo modelo para todo**: el mismo modelo sirve tanto para transcripción de archivos (batch) como para streaming en tiempo real, cuando cada caso tiene requisitos distintos

## Objetivo

Permitir que el cliente HTTP pueda especificar qué modelo quiere usar en cada petición, siempre que el modelo esté disponible en el servidor.

## Diseño propuesto

### 1. Registro automático de modelos disponibles

El servidor escaneará el directorio `models/` al iniciar y construirá un índice de modelos disponibles. No habrá lista hardcodeada — será dinámico basado en los archivos `.bin` presentes.

### 2. Peticiones HTTP: selección de modelo por parámetro

El cliente envía el campo `model` en el multipart body (como hace OpenAI):

```
POST /v1/audio/transcriptions
Content-Type: multipart/form-data

file=<audio>&model=medium
```

**Comportamiento:**
- `model` especificado y existe → usa ese modelo
- `model` especificado y **no** existe → `400 Bad Request` con lista de modelos disponibles y URL de descarga
- `model` omitido → usa un modelo por defecto (configurable via env, sugerencia: `medium`)

### 3. Endpoint de descubrimiento

```
GET /v1/models
```

Devuelve:
```json
{
  "models": [
    {"name": "tiny", "size_mb": 39, "quality": "very_low", "vrama_mb": 80},
    {"name": "small", "size_mb": 466, "quality": "medium", "vrama_mb": 500},
    {"name": "medium", "size_mb": 1536, "quality": "high", "vrama_mb": 1600},
    {"name": "medium-q5_0", "size_mb": 982, "quality": "high", "vrama_mb": 1000},
    {"name": "large-v3-turbo", "size_mb": 1550, "quality": "very_high", "vrama_mb": 1650}
  ],
  "default": "medium"
}
```

### 4. Streaming WebSocket: modelo fijo, no seleccionable

El modelo de streaming permanece fijo — se configura via `MODEL_PATH_STREAMING` (nueva variable de entorno). No se permite cambiar de modelo a mitad de una sesión WebSocket.

**Razón**: el modelo se carga cuando la sesión empieza y permanece hasta que termina. Cambiar de modelo en mitad de una sesión streaming sería complejo y no aporta valor. El caso de uso streaming prioriza latencia sobre elección de modelo.

### 5. Dos modelos por defecto (nuevas variables de entorno)

```bash
# .env — propuesta de nombres
MODEL_PATH_BATCH=/app/models/ggml-medium.bin    # HTTP (batch file transcription)
MODEL_PATH_STREAMING=/app/models/ggml-small.bin # WebSocket streaming
```

- `MODEL_PATH` sigue funcionando para backwards compatibility
- Si `MODEL_PATH_BATCH` no está definido → usa `MODEL_PATH`
- `MODEL_PATH_STREAMING` es obligatorio si se usa WebSocket

### 6. Mensaje de error para modelo no encontrado

```json
400 Bad Request
{
  "error": {
    "type": "invalid_request_error",
    "message": "Model 'large-v3' not found on this server",
    "requested_model": "large-v3",
    "available_models": ["tiny", "base", "small", "small-q5_1", "medium", "medium-q5_0", "large-v3-turbo"],
    "download_instructions": "Download models from: https://huggingface.co/ggerganov/whisper.cpp/tree/main/ggml — Place .bin files in /app/models/"
  }
}
```

## Implementación sugerida (no exhaustiva)

1. Crear `src/whisper/ModelRegistry.{h,cpp}` que escanee `models/` al init
2. Crear endpoint `GET /v1/models` en `HandleModels.cpp`
3. Modificar `HandleTranscribe::handle()` para resolver el modelo solicitado via `ModelRegistry`
4. Añadir `MODEL_PATH_BATCH` y `MODEL_PATH_STREAMING` a `ServerConfig`
5. Mantener `ModelCache` como está — ya acepta path explícito

## Out of scope

- Selección de modelo en sesiones WebSocket (futuro, no ahora)
- Descarga automática de modelos
- Modelos remotos (URLs)
- CoreML u otros backends

## Alternativas consideradas

### Opción A: Solo modelo por defecto, sin selección (STATUS QUO)
No permite al cliente elegir. Descartado porque ya existe el campo `model` en la API y los clientes lo envían esperando que funcione.

### Opción B: Lista hardcodeada de modelos
Hardcodear la lista de modelos disponibles en vez de escanear `models/`. Descartado porque requiere actualizar código cada vez que se añade un modelo y no refleja qué hay realmente en disco.

### Opción C: Variable de entorno para modelos válidos
Definir en `.env` una lista de modelos permitidos. Más complejo y propenso a errores que escanear el directorio.
