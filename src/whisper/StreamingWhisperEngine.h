#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>

// Forward declarations
struct whisper_context;
struct whisper_state;

/**
 * @brief Motor de transcripción en streaming usando whisper.cpp
 * 
 * Encapsula la API C de whisper.cpp para procesar audio en streaming.
 * Usa un whisper_context compartido (pesos, solo lectura) y un
 * whisper_state propio (estado de decodificación, thread-safe).
 *
 * Thread-safe: puede ser usado desde múltiples threads.
 */
class StreamingWhisperEngine {
public:
    /**
     * @brief Constructor con contexto compartido
     * @param shared_ctx  whisper_context ya cargado (propiedad del ModelCache)
     * @throws std::runtime_error si no se puede crear el state
     */
    explicit StreamingWhisperEngine(whisper_context* shared_ctx);
    
    ~StreamingWhisperEngine();
    
    // No copiable
    StreamingWhisperEngine(const StreamingWhisperEngine&) = delete;
    StreamingWhisperEngine& operator=(const StreamingWhisperEngine&) = delete;
    
    /**
     * @brief Agregar chunk de audio al buffer
     * @param pcm_data Audio en formato PCM float32, rango [-1.0, 1.0], 16kHz mono
     */
    void processAudioChunk(const std::vector<float>& pcm_data);
    
    
    struct TranscribeResult {
        std::string partial_text;
        std::string committed_text;
    };

    /**
     * @brief Interpreta el audio y recorta los segmentos completados de forma segura
     * @param force_commit Si es true, vuelca todo el texto a committed y vacía el buffer
     * @return `TranscribeResult` con el texto estable (commited) y el texto en vuelo (partial)
     */
    TranscribeResult transcribeSlidingWindow(bool force_commit = false);

    // Mantenemos transcribe por compatibilidad con tests (equivale a transcribeSlidingWindow(true).committed_text)
    std::string transcribe(size_t start_offset = 0);
    
    /**
     * @brief Limpiar el buffer de audio completamente o hasta un límite
     * @param keep_samples Cantidad de samples a conservar del final (ventana deslizante)
     */
    void reset(size_t keep_samples = 0);
    
    /**
     * @brief Obtener tamaño actual del buffer en samples
     */
    size_t getBufferSize() const;
    
    /**
     * @brief Configurar idioma de transcripción
     * @param lang Código de idioma (ej: "es", "en", "auto")
     */
    void setLanguage(const std::string& lang);
    
    /**
     * @brief Configurar número de threads para transcripción
     * @param n_threads Número de threads (default: 4)
     */
    void setThreads(int n_threads);

    /**
     * @brief Configurar tamaño de beam para beam search
     * @param beam_size Tamaño del beam (default: 5). 1 = greedy.
     */
    void setBeamSize(int beam_size);

    /**
     * @brief Configurar prompt inicial para guiar la transcripción
     * @param prompt Texto de contexto (ej: "Transcripción en español")
     */
    void setInitialPrompt(const std::string& prompt);

    /**
     * @brief Configurar umbral VAD (Voice Activity Detection)
     * @param vad_thold Umbral de VAD (default: 0.0f = autodetect)
     *        > 0.0 activa VAD (ej: 0.6f). Si es muy alto, recorta palabras.
     */
    void setVadThreshold(float vad_thold);

    void setTemperature(float temperature);
    void setTemperatureInc(float temperature_inc);
    void setNoSpeechThreshold(float no_speech_thold);
    void setLogprobThreshold(float logprob_thold);
    
    /**
     * @brief Verificar si el engine está listo
     */
    bool isReady() const;
    
    /**
     * @brief Convertir audio PCM int16 a float32
     */
    static std::vector<float> convertInt16ToFloat32(const std::vector<int16_t>& pcm16);
    
    /**
     * @brief Convertir bytes raw a PCM float32
     */
    static std::vector<float> convertBytesToFloat32(const std::vector<uint8_t>& bytes);

private:
    whisper_context* ctx_;       // Shared, NOT owned
    whisper_state*   state_;     // Owned, per-session
    std::vector<float> audio_buffer_;
    mutable std::mutex buffer_mutex_;
    
    // Configuration
    std::string language_;
    std::string initial_prompt_;
    int n_threads_;
    int beam_size_;
    float vad_thold_;
    float temperature_;
    float temperature_inc_;
    float no_speech_thold_;
    float logprob_thold_;
    int max_buffer_samples_; // Max samples in buffer (30s @ 16kHz)

    // High-pass filter state (per-instance, not static)
    float hp_prev_raw_      = 0.0f;
    float hp_prev_filtered_ = 0.0f;
};
