# Guía de integración — Transcription API

El servidor expone un endpoint WebSocket que recibe audio en streaming y devuelve transcripciones en tiempo real.

---

## Conexión

| Entorno | URL por defecto |
|---|---|
| Docker / producción | `ws://HOST:8003` |
| Local (sin Docker) | `ws://localhost:9001` |
| Con TLS | `wss://HOST:PUERTO` |

```
ws://192.168.1.100:8003
wss://mi-servidor.com:8003
```

Para TLS con certificado autofirmado, el cliente debe desactivar la verificación del certificado o añadir el certificado a su almacén de confianza.

---

## Flujo de sesión

```
Cliente                          Servidor
  │                                 │
  │──── WebSocket connect ─────────►│
  │                                 │
  │──── JSON: config ──────────────►│  (obligatorio antes de cualquier otra cosa)
  │◄─── JSON: ready ────────────────│
  │                                 │
  │──── Binary: audio chunk ───────►│
  │──── Binary: audio chunk ───────►│
  │◄─── JSON: transcription ────────│  (parcial, is_final: false)
  │──── Binary: audio chunk ───────►│
  │──── Binary: audio chunk ───────►│
  │◄─── JSON: transcription ────────│  (parcial, is_final: false)
  │                                 │
  │──── JSON: end ─────────────────►│
  │◄─── JSON: transcription ────────│  (final, is_final: true)
  │◄─── WebSocket close ────────────│
```

---

## Mensajes del cliente al servidor

### 1. `config` — Configuración inicial (obligatorio)

Debe ser el **primer mensaje** de la sesión. El servidor no aceptará audio hasta recibirlo.

```json
{
  "type": "config",
  "language": "es",
  "token": "TU_TOKEN"
}
```

| Campo | Tipo | Obligatorio | Descripción |
|---|---|---|---|
| `type` | string | sí | Siempre `"config"` |
| `language` | string | no | Código de idioma ISO 639-1. Default: `"es"`. Usar `"auto"` para detección automática |
| `token` | string | si el servidor tiene auth activado | Token de autenticación |
| `publish_mqtt` | boolean | no | Publicar transcripción final en MQTT. Default: `false` |
| `vad_thold` | number | no | Umbral VAD `[0.0–1.0]`. `0.0` desactiva VAD. Default: `0.0` |

**Idiomas soportados** (selección): `"es"`, `"en"`, `"fr"`, `"de"`, `"it"`, `"pt"`, `"zh"`, `"ja"`, `"ko"`, `"ru"`, `"auto"` (cualquier código soportado por Whisper).

**Respuesta esperada:** mensaje `ready` (ver sección de mensajes del servidor).

---

### 2. Audio — Chunks binarios

Después de recibir `ready`, enviar los datos de audio como **frames WebSocket binarios** (no texto).

**Formato obligatorio:**
- Codificación: `float32` little-endian
- Sample rate: **16.000 Hz**
- Canales: **mono** (1 canal)
- Rango de valores: `[-1.0, 1.0]`

**Tamaño de chunk recomendado:** 100–500 ms de audio (1.600–8.000 muestras = 6.400–32.000 bytes).

El servidor acumula audio en un buffer de máximo **30 segundos**. Existe un nivel de alerta (**high-water mark**) a los **20 segundos**: si el buffer supera ese punto el servidor descarta los nuevos chunks entrantes y envía un mensaje `warning` con `code: "buffer_full"` (una sola vez, hasta que el buffer baje del HWM). El audio más antiguo se descarta automáticamente si se supera el máximo absoluto de 30 segundos.

Las transcripciones parciales se generan automáticamente **cada vez que llega al menos 250 ms de audio nuevo** acumulado (mínimo 2 segundos de buffer para la primera inferencia).

**Conversión desde int16 (PCM estándar):**
```python
# Python / numpy
float_samples = int16_samples.astype(np.float32) / 32768.0
audio_bytes = float_samples.tobytes()  # float32 LE
```

```javascript
// JavaScript
const float32Array = new Float32Array(int16Array.length);
for (let i = 0; i < int16Array.length; i++) {
  float32Array[i] = int16Array[i] / 32768.0;
}
const bytes = float32Array.buffer;
```

---

### 3. `end` — Fin de transmisión

Indica al servidor que no hay más audio. El servidor devuelve la transcripción final de todo el buffer y cierra la conexión.

```json
{
  "type": "end"
}
```

---

## Mensajes del servidor al cliente

Todos los mensajes del servidor son **frames WebSocket de texto** con JSON.

### `ready` — Sesión configurada

Respuesta al mensaje `config` cuando todo es correcto.

```json
{
  "type": "ready",
  "protocol_version": 1,
  "session_id": "session-1709123456789-4821",
  "config": {
    "language": "es",
    "sample_rate": 16000,
    "beam_size": 1,
    "publish_mqtt": false
  }
}
```

`protocol_version` identifica la versión del protocolo WebSocket. Incrementará si se introducen cambios incompatibles. Los clientes pueden usarlo para negociar compatibilidad.

---

### `transcription` — Resultado de transcripción

```json
{
  "type": "transcription",
  "text": "Hola, esto es una prueba de transcripción.",
  "is_final": false
}
```

| Campo | Tipo | Descripción |
|---|---|---|
| `text` | string | Texto transcrito hasta el momento (acumulativo) |
| `is_final` | bool | `false` = parcial (seguirá llegando más audio). `true` = resultado definitivo tras recibir `end` |

El texto de las transcripciones parciales **incluye todo el audio procesado hasta ese momento**, no solo el chunk más reciente.

---

### `warning` — Aviso no fatal

```json
{
  "type": "warning",
  "code": "buffer_full",
  "message": "Audio buffer full, dropping incoming audio"
}
```

| `code` | Cuándo ocurre |
|---|---|
| `buffer_full` | El buffer de audio supera los 20 segundos (HWM). Los chunks entrantes se descartan hasta que el buffer se vacíe. Se envía una sola vez por episodio de saturación. |

---

### `error` — Error de sesión

```json
{
  "type": "error",
  "message": "Descripción legible del error",
  "code": "AUTH_FAILED"
}
```

| `code` | Cuándo ocurre |
|---|---|
| `AUTH_REQUIRED` | Se requiere token pero no se envió |
| `AUTH_FAILED` | El token es incorrecto |
| `NOT_CONFIGURED` | Se envió audio o `end` antes de `config` |
| `INVALID_MESSAGE` | JSON sin campo `type` |
| `UNKNOWN_TYPE` | Campo `type` con valor desconocido |
| `PARSE_ERROR` | El texto recibido no es JSON válido |
| `AUDIO_ERROR` | Error al procesar el buffer de audio |
| `CONFIG_ERROR` | Error al inicializar el motor (ej. modelo no encontrado) |

Tras un error de autenticación (`AUTH_REQUIRED`, `AUTH_FAILED`) el servidor cierra la conexión inmediatamente.

---

## Ejemplo mínimo en Python

```python
import asyncio
import websockets
import json
import numpy as np

async def transcribe(audio_float32: np.ndarray, server="ws://localhost:8003", token=None):
    async with websockets.connect(server) as ws:
        # 1. Configurar sesión
        config = {"type": "config", "language": "es"}
        if token:
            config["token"] = token
        await ws.send(json.dumps(config))

        # 2. Esperar ready
        msg = json.loads(await ws.recv())
        assert msg["type"] == "ready", f"Error: {msg}"

        # 3. Enviar audio en chunks de 500ms
        chunk_size = 8000  # 500ms @ 16kHz
        for i in range(0, len(audio_float32), chunk_size):
            chunk = audio_float32[i:i + chunk_size]
            await ws.send(chunk.tobytes())

            # Recoger transcripciones parciales si llegan
            try:
                resp = await asyncio.wait_for(ws.recv(), timeout=0.01)
                print(json.loads(resp))
            except asyncio.TimeoutError:
                pass

        # 4. Señalizar fin
        await ws.send(json.dumps({"type": "end"}))

        # 5. Recibir transcripción final
        final = json.loads(await ws.recv())
        return final["text"]
```

---

## Ejemplo mínimo en JavaScript (Node.js)

```javascript
const WebSocket = require('ws');

async function transcribe(audioFloat32, serverUrl = 'ws://localhost:8003', token = null) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(serverUrl);

    ws.on('open', () => {
      // 1. Configurar sesión
      const config = { type: 'config', language: 'es' };
      if (token) config.token = token;
      ws.send(JSON.stringify(config));
    });

    let configured = false;

    ws.on('message', (data) => {
      const msg = JSON.parse(data);

      if (msg.type === 'ready' && !configured) {
        configured = true;

        // 3. Enviar audio en chunks de 500ms (8000 muestras)
        const chunkSize = 8000;
        for (let i = 0; i < audioFloat32.length; i += chunkSize) {
          const chunk = audioFloat32.slice(i, i + chunkSize);
          ws.send(Buffer.from(chunk.buffer));
        }

        // 4. Señalizar fin
        ws.send(JSON.stringify({ type: 'end' }));

      } else if (msg.type === 'transcription') {
        if (msg.is_final) resolve(msg.text);
        else console.log('Parcial:', msg.text);

      } else if (msg.type === 'error') {
        reject(new Error(`${msg.code}: ${msg.message}`));
      }
    });

    ws.on('error', reject);
  });
}
```

---

## Conversión de audio con FFmpeg

Si el audio de origen no está en el formato requerido (float32, 16kHz, mono):

```bash
# WAV a raw float32 16kHz mono
ffmpeg -i entrada.mp3 -ar 16000 -ac 1 -f f32le salida.raw

# Verificar formato de un WAV
ffprobe -i audio.wav
```

---

## Cliente de prueba incluido

El repositorio incluye un cliente de prueba en `clients/test_client.py`:

```bash
pip install websockets numpy sounddevice

# Desde archivo WAV
python clients/test_client.py --file audio.wav --url ws://localhost:8003

# Desde micrófono
python clients/test_client.py --mic --url ws://localhost:8003

# Con autenticación
python clients/test_client.py --file audio.wav --token MI_TOKEN

# Audio sintético (para probar sin audio real)
python clients/test_client.py --generate silence --duration 5
```
