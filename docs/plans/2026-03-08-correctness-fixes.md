# Correctness Fixes (Option A) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix three correctness issues: wrong audio frame validation, missing max-frame-size guard, and a broken graceful shutdown that calls `sleep+exit` instead of joining threads.

**Architecture:** Three independent changes to `StreamingSession.h` (frame validation + rate limit guard) and `src/server.cpp` + `ServerConfig.h` (shutdown). No new files except a `ServerConfig` field. Shutdown uses a detached watchdog thread to enforce the timeout without platform-specific APIs.

**Tech Stack:** C++17, Boost.Asio, Boost.Beast, Google Test (existing)

---

### Task 1: Fix audio frame validation (Bug A)

**Files:**
- Modify: `src/server/StreamingSession.h` (~line 255)

**Step 1: Leer el bloque actual**

En `handleBinaryMessage`, localizar:
```cpp
if (data.size() < 44) { // WAV header is 44 bytes, so anything smaller is not a valid audio file
    Log::warn("Received binary data too small to be a WAV file (" + std::to_string(data.size()) + " bytes)", session_id_);
    return;
}
```
El servidor **no acepta WAV** — acepta float32 raw. Un chunk válido de menos de 44 bytes (ej. 10 muestras = 40 bytes) es descartado silenciosamente por un comentario incorrecto.

**Step 2: Aplicar el fix**

Reemplazar ese bloque con:
```cpp
if (data.size() < sizeof(float)) {
    Log::warn("Binary frame too small for float32 (" + std::to_string(data.size()) + " bytes), ignoring", session_id_);
    return;
}
```

**Step 3: Compilar para verificar**

```bash
cmake --build build --target unit_tests -j$(nproc) 2>&1 | tail -5
cmake --build build --target transcription_server -j$(nproc) 2>&1 | tail -5
```

Esperado: compila sin errores ni warnings nuevos.

**Step 4: Commit**

```bash
git add src/server/StreamingSession.h
git commit -m "fix: correct audio frame minimum size check (float32, not WAV header)"
```

---

### Task 2: Fix rate limit comment + añadir max frame size

**Files:**
- Modify: `src/server/StreamingSession.h` (función `handleBinaryMessage`, ~línea 234)

**Step 1: Leer el bloque de rate limiting**

El bloque actual comienza con:
```cpp
void handleBinaryMessage(const std::vector<unsigned char>& data) {
    // Enforce Binary Rate Limit (QoS)
    auto now = std::chrono::steady_clock::now();
    ...
    if (elapsed_s >= 3) {
        // max 200 KB/s over 3 seconds = 600 KB window limit
```
El comentario dice "sliding" implícitamente pero es una ventana fija de 3s. Además no hay límite de tamaño por frame individual.

**Step 2: Añadir el guard de max frame size al inicio de `handleBinaryMessage`**

Insertar **como primera comprobación** de la función (antes del rate limit), justo después de la llave de apertura de `handleBinaryMessage`:

```cpp
    // Guard: reject oversized frames immediately — 1 MB = ~62s of float32 audio @ 16kHz,
    // far beyond any legitimate streaming chunk.
    constexpr size_t MAX_FRAME_BYTES = 1 * 1024 * 1024; // 1 MB
    if (data.size() > MAX_FRAME_BYTES) {
        Log::warn("Binary frame too large (" + std::to_string(data.size()) +
                  " bytes > 1MB limit), closing connection", session_id_);
        boost::system::error_code ec;
        ws_.close(websocket::close_reason(websocket::close_code::policy_error, "Frame too large"), ec);
        return;
    }
```

**Step 3: Corregir el comentario del rate limit**

Cambiar:
```cpp
        // max 200 KB/s over 3 seconds = 600 KB window limit
```
Por:
```cpp
        // Fixed 3-second window: max 600 KB per window (~200 KB/s average).
        // Note: this is a fixed window, not sliding — resets every 3 seconds.
```

**Step 4: Compilar y ejecutar tests**

```bash
cmake --build build --target unit_tests -j$(nproc) 2>&1 | tail -5
./build/tests/unit_tests 2>&1 | tail -5
```

Esperado: compila sin errores, 60 tests pasan.

**Step 5: Commit**

```bash
git add src/server/StreamingSession.h
git commit -m "fix: add 1MB max frame size guard + correct rate limit comment"
```

---

### Task 3: Graceful shutdown con join explícito de threads

**Files:**
- Modify: `src/server/ServerConfig.h`
- Modify: `src/server.cpp`

**Contexto del problema:**

El estado actual:
```cpp
// Threads de sesión se lanzan como detached — no hay forma de joinarlos
std::thread(handleSession, ...).detach();

// Signal handler hace sleep(2s) + exit(0) sin esperar a que los threads terminen
signals.async_wait([&](...) {
    acceptor.close();
    SessionTracker::instance().shutdownAll();
    std::this_thread::sleep_for(std::chrono::seconds(2)); // hack
    std::exit(0); // mata el proceso sin join
});
```

Además, `ioc.run()` **nunca se llama**, por lo que el `signals.async_wait` podría no dispararse por la vía normal de Boost.Asio. La señal llega porque SIGINT interrumpe el `acceptor.accept()` bloqueante, pero el handler registrado en `ioc` nunca ejecuta a menos que `ioc.run()` o `ioc.poll()` sean llamados.

**Diseño de la solución:**
1. Ejecutar `ioc` en un thread separado (solo para signal handling)
2. Signal handler: cerrar acceptor + llamar `shutdownAll()` (cancela sockets activos)
3. Accept loop: acumular threads en un `std::vector<std::thread>` en lugar de `.detach()`
4. Después del accept loop: join de todos los threads con watchdog de timeout

**Step 1: Añadir `shutdown_timeout_sec` a `ServerConfig.h`**

Al final del struct `ServerConfig`, añadir:
```cpp
    int shutdown_timeout_sec = 10;      // max seconds to wait for sessions to close on SIGINT/SIGTERM
```

**Step 2: Añadir env var en `configFromEnv()` en `server.cpp`**

Añadir al final del bloque de ifs en `configFromEnv()`, antes del `return cfg;`:
```cpp
    if (auto v = env("SHUTDOWN_TIMEOUT_SEC"); !v.empty())
        cfg.shutdown_timeout_sec = std::stoi(v);
```

**Step 3: Añadir CLI arg en `parseArgs()` en `server.cpp`**

Añadir en el bloque de parsing de argumentos, junto a `--session-timeout-sec`:
```cpp
        } else if (arg == "--shutdown-timeout-sec" && i + 1 < argc) {
            config.shutdown_timeout_sec = std::stoi(argv[++i]);
```

**Step 4: Actualizar `printUsage()` en `server.cpp`**

Añadir `--shutdown-timeout-sec N` en la línea del usage:
```cpp
              << " [--session-timeout-sec N] [--shutdown-timeout-sec N]"
```
Y en la lista de variables de entorno:
```cpp
    std::cout << "  SESSION_TIMEOUT_SEC, SHUTDOWN_TIMEOUT_SEC" << std::endl;
```

**Step 5: Refactorizar el main loop en `server.cpp`**

Localizar en `main()` el bloque que va desde `boost::asio::signal_set signals(ioc, ...)` hasta `.detach()`.

Reemplazar TODO ese bloque (signal handler + accept loop) con:

```cpp
        // Thread vector: collect all session threads so we can join them at shutdown.
        // Sessions are bounded by max_connections (default 8), so the vector is small.
        std::vector<std::thread> session_threads;

        // Run ioc in a dedicated thread so async_wait fires even while accept() blocks.
        std::thread ioc_thread([&ioc]() { ioc.run(); });

        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](boost::system::error_code const&, int signum) {
            Log::info("Signal " + std::to_string(signum) + " received — stopping accept loop");
            acceptor.close();
            SessionTracker::instance().shutdownAll();
        });

        while (acceptor.is_open()) {
            tcp::socket socket(ioc);
            boost::system::error_code ec;
            acceptor.accept(socket, ec);

            if (ec) {
                if (ec == boost::asio::error::operation_aborted) break;
                Log::error("Accept error: " + ec.message());
                continue;
            }

            std::string client_ip = socket.remote_endpoint().address().to_string();

            if (!limiter->tryAcquire(client_ip)) {
                Log::warn("Connection rejected (limit reached): " + client_ip);
                socket.close();
                continue;
            }

            Log::info("New connection from " + client_ip);

            try {
                session_threads.emplace_back(handleSession,
                            std::move(socket),
                            limiter,
                            client_ip,
                            config.model_path,
                            auth_manager,
                            config.whisper_beam_size,
                            config.whisper_threads,
                            config.whisper_initial_prompt,
                            config.session_timeout_sec,
                            config.whisper_temperature,
                            config.whisper_temperature_inc,
                            config.whisper_no_speech_thold,
                            config.whisper_logprob_thold,
                            ssl_ctx,
                            mqtt_publisher);
            } catch (...) {
                limiter->release(client_ip);
                throw;
            }
        }

        // Accept loop exited — all sockets already cancelled by shutdownAll().
        // Join session threads with a configurable timeout via a watchdog.
        Log::info("Waiting up to " + std::to_string(config.shutdown_timeout_sec) +
                  "s for " + std::to_string(session_threads.size()) + " session(s) to finish...");

        std::atomic<bool> all_joined{false};
        std::thread watchdog([&all_joined, timeout = config.shutdown_timeout_sec]() {
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(timeout);
            while (!all_joined) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    Log::warn("Shutdown timeout (" + std::to_string(timeout) +
                              "s) exceeded, forcing exit");
                    std::exit(0);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        watchdog.detach();

        for (auto& t : session_threads) {
            if (t.joinable()) t.join();
        }
        all_joined = true;

        ioc.stop();
        if (ioc_thread.joinable()) ioc_thread.join();

        Log::info("Graceful shutdown complete.");
```

**Step 6: Compilar servidor completo**

```bash
cmake -B build -DBUILD_TESTS=ON -DBUILD_SERVER=ON -DBUILD_SHARED_LIBS=OFF 2>&1 | tail -3
cmake --build build -j$(nproc) 2>&1 | tail -5
```

Esperado: compila sin errores.

**Step 7: Verificar shutdown manual**

```bash
# Arrancar servidor en background
./build/transcription_server --model third_party/whisper.cpp/models/ggml-small.bin &
SERVER_PID=$!
sleep 1

# Enviar SIGTERM y verificar que sale limpiamente (sin "Killed" del OS)
kill -TERM $SERVER_PID
wait $SERVER_PID
echo "Exit code: $?"
```

Esperado: el servidor imprime `"Graceful shutdown complete."` y sale con código 0.

**Step 8: Ejecutar suite completa de tests**

```bash
./build/tests/unit_tests 2>&1 | tail -5
```

Esperado: 60 tests pasan.

**Step 9: Commit**

```bash
git add src/server/ServerConfig.h src/server.cpp
git commit -m "fix: graceful shutdown with explicit thread join and configurable timeout"
```
