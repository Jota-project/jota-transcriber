# Guía de Integración: Transcriptor -> Orquestador vía MQTT

Esta guía detalla cómo conectar el servicio de Transcripción C++ con el Orquestador de IA utilizando MQTT. 
La idea principal es que `jota-transcriber` actúe como "Productor" (Publicador) enviando los textos finales, y el Orquestador actúe como "Consumidor" (Suscriptor) procesándolos.

## 1. Configuración del Transcriptor (Productor)

El servidor C++ ya tiene soporte nativo para conectarse a un broker MQTT. Para activarlo, asegúrate de que el `.env` del Transcriptor tenga configuradas las siguientes variables:

```env
# Variables en jota-transcriber (.env)
MQTT_URL=mqtt://tu-broker:1883
MQTT_TOPIC=transcription/results
MQTT_CLIENT_ID=jota-transcriber
```

> [!NOTE]
> `jota-transcriber` publicará automáticamente eventos **solo cuando la transcripción es final** (`is_final = true`), es decir, cuando la sesión del WebSocket se cierra tras el envío del flag `end`.

## 2. Estructura del Evento (`TranscriptionEvent`)

El Orquestador recibirá un payload en formato JSON limpio con la siguiente estructura:

```json
{
  "session_id": "session-1715421251341-8912",
  "text": "Hola, me gustaría agendar una cita para mañana.",
  "is_final": true,
  "language": "es",
  "timestamp_ms": 1715421251341
}
```

### Campos Clave para el Orquestador:
- `session_id`: El identificador único de la conexión/sesión. Usa este ID en tu Orquestador de IA para mantener el contexto del agente, enlazarlo con una llamada telefónica o websocket de cliente, o recuperar el historial exacto del usuario.
- `text`: El string resultante de Whisper que el LLM del Orquestador procesará como el _prompt_ o _user_input_.
- `language`: El idioma detectado (útil si el orquestador necesita pre-ajustar su _system prompt_ a un idioma).

## 3. Implementación en el Orquestador (Consumidor)

Si tu orquestador está construido en Python, Node.js u otro lenguaje, puedes usar una librería estándar de MQTT (por ejemplo, `paho-mqtt` en Python). 

Aquí tienes un ejemplo de cómo implementar el consumidor en Python:

### Instalación de dependencias (Python)
```bash
pip install paho-mqtt
```

### Código Base del Suscriptor (Orquestador)

```python
import paho.mqtt.client as mqtt
import json

# Configuración del Broker y Topic (debe coincidir con jota-transcriber)
BROKER_URL = "tu-broker"
BROKER_PORT = 1883
TOPIC = "transcription/results"

def on_connect(client, userdata, flags, rc):
    print(f"Orquestador conectado a MQTT (Código: {rc}). Esperando transcripciones...")
    # Nos suscribimos al tópico que jota-transcriber usa para publicar
    client.subscribe(TOPIC)

def on_message(client, userdata, msg):
    try:
        # 1. Recibir y decodificar el payload JSON
        payload = json.loads(msg.payload.decode('utf-8'))
        
        session_id = payload.get("session_id")
        texto = payload.get("text")
        
        print(f"-> [Nueva Transcripción] Sesión: {session_id} | Texto: '{texto}'")
        
        # 2. Inyectar al Pipeline de IA
        # Aquí llamas a tu Lógica LLM. Ej:
        # ai_response = agent_orchestrator.process_message(session_id, texto)
        # play_audio_response(ai_response)
        
    except json.JSONDecodeError:
        print("Error: Payload recibido no es JSON válido.")

# Inicialización del cliente MQTT
client = mqtt.Client(client_id="ai_orchestrator")
client.on_connect = on_connect
client.on_message = on_message

print(f"Conectando al broker {BROKER_URL}:{BROKER_PORT}...")
client.connect(BROKER_URL, BROKER_PORT, 60)

# Bucle infinito escuchando nuevos mensajes
client.loop_forever()
```

## 4. Flujo Completo y Latencia

1. **Cliente a jota-transcriber:** El cliente inicia el WebSocket e inyecta fragmentos de audio en tiempo real. 
2. **Fin de Sesión:** El cliente envía el comando JSON `{"type": "end"}` (o corta la conexión).
3. **MQTT Publish:** En milisegundos, el motor de C++ consolida el buffer `res.committed_text` en `full_transcription_` y publica el payload JSON en el topic `transcription/results`.
4. **Orquestador:** El script de Python/Node del Orquestador se dispara mediante la callback de suscripción MQTT, leyendo el mensaje e integrándolo instantáneamente a langchain o a tu flow de OpenAI.

> [!TIP]
> **Vinculando WebSockets y MQTT:** Los clientes a veces necesitan enviar Metadatos extras (como IDs de cuenta de cliente, token JWT...) que se necesitan en el orquestador. El `session_id` actua como un puente unificador natural. El cliente original inicia la LLamada con ese `session_id`, el motor lo reporta a MQTT, y el orquestador actualiza la base de datos usando ese mismo `session_id` como clave.
