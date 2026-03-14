# Clientes de Prueba

Este directorio contiene clientes de ejemplo para probar el servidor de transcripción en streaming.

## Cliente Python

### Requisitos

```bash
pip install websockets numpy sounddevice
```

### Uso

El cliente soporta tres modos de operación:

#### 1. Archivo de Audio
Envía un archivo WAV existente:
```bash
python test_client.py --file <archivo.wav>
```

#### 2. Micrófono
Transmite audio en tiempo real desde el micrófono predeterminado:
```bash
python test_client.py --mic
```

#### 3. Generación de Audio
Genera señales de prueba para verificar robustez y VAD:
```bash
# Silencio (Testear VAD)
python test_client.py --generate silence --duration 5

# Ruido Blanco (Testear Robustez)
python test_client.py --generate noise --duration 5

# Tono Puro (Testear Procesamiento de Señal)
python test_client.py --generate tone --freq 440
```

### Opciones Comunes

- `--url`: URL del servidor (default: `ws://localhost:9001`)
- `--duration`: Duración en segundos para generación (default: 5.0)
- `--freq`: Frecuencia en Hz para tonos (default: 440.0)

### Formato de Audio

El cliente acepta archivos WAV con las siguientes características:
- **Canales**: Mono (1 canal) - si es estéreo, usará solo el primer canal
- **Sample Rate**: 16kHz (recomendado) - otros rates funcionan pero pueden afectar la precisión
- **Formato**: int16 o float32

### Convertir Audio con FFmpeg

Si tienes un archivo de audio en otro formato, puedes convertirlo con FFmpeg:

```bash
# Convertir cualquier audio a WAV 16kHz mono
ffmpeg -i input.mp3 -ar 16000 -ac 1 -sample_fmt s16 output.wav

# Desde un video
ffmpeg -i video.mp4 -ar 16000 -ac 1 -sample_fmt s16 audio.wav
```

### Protocolo

El cliente implementa el protocolo JSON del servidor:

1. **Configuración** (primer mensaje obligatorio):
```json
{
  "type": "config",
  "language": "es",
  "token": "TU_TOKEN",
  "vad_thold": 0.0
}
```
`token` solo es obligatorio si el servidor tiene auth activado. `language` y `vad_thold` son opcionales.

2. **Audio** (frames WebSocket binarios, NO JSON):

Los datos de audio se envían como **frames WebSocket binarios**: float32 little-endian, 16 kHz, mono. Chunks recomendados de 100–500 ms (1 600–8 000 muestras = 6 400–32 000 bytes).

3. **Finalización**:
```json
{
  "type": "end"
}
```

### Respuestas del Servidor

- **Ready**: Confirmación de configuración con `session_id` y parámetros activos
- **Transcription**: Texto transcrito acumulativo (`is_final: false` parcial, `is_final: true` tras `end`)
- **Warning**: Aviso no fatal, ej. `"code": "buffer_full"` cuando el buffer supera 20 s
- **Error**: Error de sesión con `code` (ver `clients/API_GUIDE.md` para lista completa)

## Cliente Web (Próximamente)

Cliente HTML/JavaScript para capturar audio del micrófono en tiempo real.

## Ejemplo de Salida

```
🔌 Conectando a ws://localhost:9001...
✓ Conectado
⚙️  Enviando configuración: {'type': 'config', 'language': 'es', ...}
✓ Servidor listo: {'type': 'ready', 'session_id': 'session-1234567890-5678', ...}
📁 Cargando audio: test.wav
   Formato: 1 canal(es), 16000Hz, 16bits
📤 Enviando audio en chunks de 100ms (1600 samples)...
🔴 Transcripción: Hola mundo
🔴 Transcripción: Cómo estás
✓ Enviados 50 chunks
🏁 Mensaje de finalización enviado
👋 Desconectado
```
