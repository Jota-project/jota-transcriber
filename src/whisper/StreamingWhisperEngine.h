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
    
    /**
     * @brief Transcribir el buffer acumulado
     * @return Texto transcrito
     * @throws std::runtime_error si la transcripción falla
     */
    std::string transcribe();
    
    /**
     * @brief Limpiar el buffer de audio
     */
    void reset();
    
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
    int max_buffer_samples_; // Max samples in buffer (30s @ 16kHz)
};
