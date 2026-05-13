# Configurar OpenClaw para usar jota-transcriber como backend de transcripción

Esta guía explica cómo configurar OpenClaw para que envíe los audios de voz recibidos (por ejemplo, notas de voz de Telegram) directamente a tu servidor **jota-transcriber** en vez de usar OpenAI Whisper API, Whisper local, o cualquier otro servicio externo.

---

## Qué consigues

- **Transcripción 100% local** — sin llamadas a APIs externas, sin coste de API key
- **Rendimiento optimizado** — whisper.cpp C++ compilado con enlaces estáticos, modelo cargado en memoria
- **API compatible con OpenAI** — jota-transcriber expone el endpoint `POST /v1/audio/transcriptions` que es drop-in replacement del endpoint de OpenAI
- **Autenticación integrada** — el servidor valida token Bearer, así que las credenciales no salen de tu red

---

## Requisitos previos

1. **Servidor jota-transcriber corriendo** en algún endpoint accesible (localhost, IP local, o dominio público)
2. **Token de autenticación** del servidor (el que usas con `--auth-token` al iniciar el servidor)
3. **OpenClaw** instalado y funcionando
4. **OpenClaw CLI** accesible en PATH (`openclaw`)

---

## Paso 1 — Verificar que el servidor responde

Antes de configurar nada, asegúrate de que el servidor está accesible y accepting requests:

```bash
curl -s -X POST http://<HOST>:<PORT>/v1/audio/transcriptions \
  -H "Authorization: Bearer <TU_TOKEN>" \
  -F "file=@/ruta/a/tu/audio.m4a" \
  -F "model=whisper-1" \
  -F "response_format=text"
```

Deberías recibir la transcripción en texto plano. Si no, revisa que el servidor esté corriendo y que el token sea correcto.

---

## Paso 2 — Obtener el token del servidor

El token es el que pasaste al arrancar jota-transcriber con `--auth-token`. Si lo tienes en una variable de entorno (por ejemplo `AUTH_TOKEN` en el `.env`), anótalo.

Si el servidor corre en local sin auth, puedes omitir el header `Authorization` y usar un token arbitrario (el servidor lo rechazará si tiene auth activada).

---

## Paso 3 — Configurar OpenClaw

Abre la CLI de OpenClaw y ejecuta:

```bash
openclaw config set tools.media.audio '{"enabled":true,"models":[{"provider":"openai","model":"whisper-1","headers":{"Authorization":"Bearer <TU_TOKEN>"},"baseUrl":"http://<HOST>:<PORT>/v1"}]}'
```

Sustituye:
- `<TU_TOKEN>` — tu token de autenticación del servidor
- `<HOST>` — dirección del servidor (ej. `127.0.0.1` si corre en local, o `192.168.1.100`, o tu dominio)
- `<PORT>` — puerto del servidor (por defecto `8003`)

### Ejemplo con servidor en local

```bash
openclaw config set tools.media.audio '{"enabled":true,"models":[{"provider":"openai","model":"whisper-1","headers":{"Authorization":"Bearer testtoken"},"baseUrl":"http://127.0.0.1:8003/v1"}]}'
```

### Ejemplo con servidor en otra máquina de la red

```bash
openclaw config set tools.media.audio '{"enabled":true,"models":[{"provider":"openai","model":"whisper-1","headers":{"Authorization":"Bearer m1c0t0k3n"},"baseUrl":"http://192.168.1.100:8003/v1"}]}'
```

---

## Paso 4 — Reiniciar el gateway

OpenClaw necesita reiniciar el gateway para aplicar los cambios:

```bash
openclaw gateway restart
```

Espera unos segundos a que el servicio vuelva a estar disponible.

---

## Cómo funciona por dentro

OpenClaw usa `tools.media.audio` para transcripts de voz entrantes. Cuando detecta un audio (nota de voz de Telegram, audio de Discord, etc.), sigue esta cadena:

1. Descarga el audio y lo guarda como archivo temporal
2. Construye una petición multipart/form-data hacia `POST /v1/audio/transcriptions`
3. Usa el `baseUrl` + `/audio/transcriptions` como endpoint
4. Pasa el `model`, el `file`, y el header `Authorization` tal cual
5. Recibe la transcripción y la inyecta en el contexto del agente como `{{Transcript}}`

El servidor jota-transcriber procesa el audio con whisper.cpp y devuelve exactamente la misma estructura de respuesta que OpenAI Whisper API, así que OpenClaw no sabe (ni necesita saber) que el backend es local.

---

## Desactivar la configuración

Si quieres volver al comportamiento por defecto de OpenClaw (autodetección de providers o Whisper local):

```bash
openclaw config set tools.media.audio '{"enabled":false}'
openclaw gateway restart
```

---

## Notas adicionales

### Puerto por defecto

El puerto por defecto de jota-transcriber es `9001` en local y `8003` cuando corre dentro del contenedor Docker. Asegúrate de usar el correcto según cómo arranques el servidor.

### Modelo

El campo `model=whisper-1` es un placeholder requerido por la API de OpenAI. jota-transcriber lo ignora — usa el modelo que tengas cargado en el servidor (especificado con `--model` al arrancar).

### Idiomas

Puedes especificar el idioma enviando el campo `language` en la request. jota-transcriber lo soporta nativamente. OpenClaw no envía `language` por defecto (usa `"auto"`), pero podrías configurar un prompt inicial o forzar un idioma si lo necesitas.

### Tamaños de audio

OpenClaw limita por defecto a 20MB por audio (`tools.media.audio.maxBytes`). jota-transcriber tiene su propio límite configurable via `--max-upload-bytes` (25MB por defecto). Si necesitas archivos más grandes, ajusta ambos.

### Auth del servidor

Si arranques jota-transcriber **sin** `--auth-token`, el servidor no requerirá token. En ese caso puedes usar cualquier string en el `Authorization` header (OpenClaw lo enviará pero el servidor lo ignorará).

---

## Solución de problemas

| Problema | Causa probable | Solución |
|---|---|---|
| `401 Unauthorized` | Token incorrecto o no coincide | Verifica el token en el servidor y en `openclaw config` |
| `Connection refused` | Servidor no accesible desde donde corre OpenClaw | Comprueba IP:puerto, firewall, y que el servidor esté corriendo |
| `400 Bad Request` | El audio no es formato válido o falta `model=whisper-1` | Asegúrate de enviar un archivo de audio real y el campo `model` |
| `413 Payload Too Large` | Audio supera el límite del servidor | Aumenta `--max-upload-bytes` en jota-transcriber o reduce el tamaño del audio |

Para verificar la config aplicada:

```bash
openclaw config get tools
```

Para ver logs del gateway:

```bash
openclaw logs --tail 50
```
