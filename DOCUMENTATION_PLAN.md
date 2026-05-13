# Plan de documentación — Endpoint REST estilo OpenAI

Fecha: 2026-05-12
Objetivo: Documentar el nuevo endpoint `POST /v1/audio/transcriptions` en todos los lugares relevantes del repositorio para que la comunidad sepa que existe y sepa cómo usarlo.

---

## Estado actual de la documentación

### README.md — ⚠️ Necesita cambios importantes

**Línea 8 (tagline):**
> "High-performance real-time audio transcription microservice built with C++17 and [whisper.cpp](https://github.com/ggerganov/whisper.cpp). **Clients stream raw PCM audio over WebSocket**..."

**Problema:** Solo menciona WebSocket. No dice que también hay API REST HTTP estilo OpenAI.

**Cambio propuesto:**
- Actualizar la descripción de una línea para mencionar ambos protocolos
- Sugerencia: "High-performance audio transcription microservice with **real-time WebSocket streaming** and a **drop-in OpenAI-compatible REST API**, built with C++17 and whisper.cpp."

**Línea 12 (Features list):**
> "- Real-time streaming with partial transcriptions while audio is being sent"
> "- WebSocket (WS) and WebSocket over TLS (WSS)"

**Problema:** Toda la lista de features solo describe WebSocket. No hay feature para el endpoint REST.

**Cambio propuesto:**
- Añadir feature: "OpenAI-compatible REST API (`POST /v1/audio/transcriptions`) — same format as OpenAI Whisper API, works with any OpenAI-compatible client"
- Opcional: "File-based transcription via multipart/form-data for simple integrations"

**Líneas 190-198 (HTTP Endpoints):**
> "| `GET /health` | Returns `{"status": "ok"}`..."
> "| `GET /ready` | Returns `{"status": "ready"}`..."
> "| `GET /metrics` | Prometheus text format..."

**Problema:** Solo lista 3 endpoints de gestión. El endpoint de transcripción REST no aparece.

**Cambio propuesto:**
- Añadir una sección nueva `## REST API (OpenAI-compatible)` antes o después de `## WebSocket Protocol`
- O incluir el endpoint en la tabla de HTTP Endpoints:
  `POST /v1/audio/transcriptions | OpenAI-compatible file transcription | multipart/form-data`
- Renombrar la sección existente `## HTTP Endpoints` a algo como `## Management Endpoints` para dejar claro que son solo health/metrics

**Líneas 125-128 (WebSocket Protocol):**
> "Full protocol documentation in `clients/API_GUIDE.md`."

**Problema:** Solo hace referencia al docs del protocolo WebSocket.

**Cambio propuesto:**
- Añadir referencia análoga para la REST API: "For the OpenAI-compatible REST API, see `clients/API_GUIDE.md#rest-api`."

---

### CLAUDE.md — ⚠️ Necesita cambios

**Primer párrafo:**
> "Real-time audio transcription microservice in C++17 using OpenAI Whisper (`whisper.cpp`). **Clients stream raw PCM audio over WebSocket**..."

**Problema:** La descripción para Claude Code (el desarrollador) no menciona que existe modo REST.

**Cambio propuesto:**
- Añadir mención a ambos protocolos: "...or use the OpenAI-compatible REST API for simple file-based transcription."
- O añadir una nota: "Supports two protocols: WebSocket streaming (`src/server/StreamingSession.h`) and REST HTTP (`src/server/handlers/HandleTranscribe.cpp`)"

**Sección Running the Server:**
> Muestra ejemplo con `--port 8003` pero sin mención a auth ni a la diferencia Docker vs local.

**Problema:** El token de auth (`--auth-token`) es necesario para el endpoint REST si está activado. No se menciona.

**Cambio propuesto:**
- Añadir nota sobre auth: "The REST endpoint requires `Authorization: Bearer <token>` if `--auth-token` is set."

---

### clients/API_GUIDE.md — ⚠️ Necesita cambios (es el más importante)

**Título:**
> "Guía de integración — Transcription API"

**Problema:** La palabra "API" sugiere que cubre todo, pero solo explica WebSocket. Quien busque "REST API" o "OpenAI" no sabrá que está aquí.

**Cambio propuesto:**
- Añadir una nota clara al inicio: "El servidor soporta dos protocolos: **WebSocket** (streaming en tiempo real, ver sección 1) y **REST HTTP** (transcripción de archivos, ver sección 2). El endpoint REST es compatible con la API de OpenAI Whisper."
- O dividir el documento en dos partes claramente separadas

**Sección Conexión:**
> Solo menciona URLs WS (`ws://`, `wss://`).

**Problema:** Quien quiera usar REST no sabe qué URL usar.

**Cambio propuesto:**
- Añadir tabla o sección para REST:
  ```
  # REST endpoint (OpenAI-compatible)
  http://HOST:PORT/v1/audio/transcriptions
  ```
- Especificar que es `POST` multipart/form-data

**Falta sección completa para REST:**
No existe documentación del flujo REST, los campos del multipart, los formatos de respuesta, los códigos de error REST, ni ejemplos con curl/Python requests.

**Cambio propuesto:** Añadir después de la sección de WebSocket:

```
## 2. REST API (OpenAI-compatible)

El servidor expone `POST /v1/audio/transcriptions` con el mismo formato que la API de OpenAI Whisper.

### Endpoint

POST /v1/audio/transcriptions

### Autenticación

Header `Authorization: Bearer <token>` — obligatorio si el servidor se arranca con `--auth-token`.

### Campos multipart/form-data

| Campo | Tipo | Obligatorio | Descripción |
|---|---|---|---|
| `file` | file | sí | Archivo de audio (cualquier formato que FFmpeg soporte) |
| `model` | string | sí | Placeholder — se ignora, el modelo se carga desde `--model` |
| `language` | string | no | Código ISO 639-1. Default: `auto` |
| `response_format` | string | no | `json` (default), `text`, `verbose_json` |
| `temperature` | float | no | 0.0–1.0. Default: del servidor |
| `prompt` | string | no | Texto inicial para guiar la transcripción |
| `task` | string | no | `transcribe` (default) o `translate` |

### Códigos de respuesta HTTP

| Código | Significado |
|---|---|
| 200 | Transcripción exitosa |
| 400 | Parámetros inválidos o audio no decodificable |
| 401 | Token inválido o faltante |
| 413 | Audio demasiado grande (supera `--max-upload-bytes`) |
| 503 | Servidor en máxima capacidad de inferencia |

### Ejemplo con curl

curl -s -X POST http://localhost:8003/v1/audio/transcriptions \
  -H "Authorization: Bearer testtoken" \
  -F "file=@audio.m4a" \
  -F "model=whisper-1" \
  -F "response_format=text"

### Ejemplo con Python (requests)

import requests

resp = requests.post(
    "http://localhost:8003/v1/audio/transcriptions",
    headers={"Authorization": "Bearer testtoken"},
    files={"file": ("audio.m4a", open("audio.m4a", "rb"), "audio/mp4")},
    data={"model": "whisper-1", "response_format": "text"}
)
print(resp.json()["text"])
```

---

### clients/README.md — ⚠️ Necesita cambios menores

**Título:**
> "Clientes de Prueba"

**Problema:** Solo menciona el cliente WebSocket de streaming. No dice que hay otro modo de usar el servidor.

**Cambio propuesto:**
- Añadir nota al inicio: "El servidor también puede usarse mediante REST API (OpenAI-compatible). Para detalles, ver `API_GUIDE.md#rest-api`."
- El contenido actual del README está bien para el cliente WebSocket, pero debería referenciar que existe la otra opción.

---

### docker-compose.yml — ℹ️ Solo comentario opcional

**Estado actual:** No tiene comentarios sobre el endpoint REST.

**Cambio propuesto (opcional, no crítico):**
- Añadir comentario en el `env_file` o como variable de entorno documentada:
  ```yaml
  # AUTH_TOKEN: token para el endpoint REST (requerido si el servidor tiene --auth-token activo)
  ```
- No es necesario cambiar la estructura del archivo, solo hacerlo más claro.

---

### CHANGELOG.md — ✅ No necesita cambios

El changelog ya tiene la entrada correcta en la versión 1.1.0:
> "add POST /v1/audio/transcriptions (OpenAI-compatible)"

No requiere modificación. El formato es correcto y ya menciona que es OpenAI-compatible.

---

### OPENCLAW_SETUP.md — ✅ Ya creado, no cambiar

Este archivo ya está en la raíz del repo y explica exactamente cómo configurar OpenClaw para usar jota-transcriber como backend. Cumple su objetivo. Lo único que habría que decidir es si debe moverse a `docs/` o mantenerse en la raíz.

---

## Resumen de cambios por archivo

| Archivo | Urgencia | Tipo de cambio |
|---|---|---|
| `README.md` | 🔴 Alta | Añadir feature REST, actualizar tagline, ampliar HTTP Endpoints, añadir referencia a REST en WebSocket section |
| `CLAUDE.md` | 🟡 Media | Actualizar descripción, añadir nota de auth para REST |
| `clients/API_GUIDE.md` | 🔴 Alta | Añadir sección completa de REST con ejemplos curl/Python, actualizar intro y tabla de URLs |
| `clients/README.md` | 🟢 Baja | Añadir nota referenciando la REST API |
| `docker-compose.yml` | 🟢 Baja | Comentario opcional sobre AUTH_TOKEN |
| `CHANGELOG.md` | ✅ Ninguna | — |
| `OPENCLAW_SETUP.md` | ✅ Ninguna | — |

---

## Recomendación de orden de trabajo

1. **`clients/API_GUIDE.md`** — Es el doc más consulted por desarrolladores que quieren integrar. Prioridad absoluta.
2. **`README.md`** — Es la primera página que ve cualquier persona. Actualizar tagline + features + HTTP endpoints table.
3. **`CLAUDE.md`** — Relevante solo para quienes trabajen en el código. Cambio menor.
4. **`clients/README.md`** — Cambio trivial, bajo impacto.
5. **`docker-compose.yml`** — Opcional, bajo prioridad.

---

## Notas adicionales

- El endpoint REST soporta `verbose_json` con `id`, `created_at`, `task`, `language`, `duration`, `text`, `segments` — habría que documentar esto si se considera importante para consumidores de la API.
- El campo `task=translate` convierte al inglés — está implementado pero no hay referencia a ello en ningún doc.
- El `response_format=srt` y `response_format=vtt` devolvían 400 según la review #39 — decidir si se quiere soportar o documentar como no soportado.
- No hay ejemplos de uso del endpoint REST con `curl` ni `fetch` en ningún doc — el ejemplo mínimo de Python con `requests`填补 este gap.
