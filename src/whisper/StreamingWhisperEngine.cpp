#include "StreamingWhisperEngine.h"
#include <whisper.h>
#include <stdexcept>
#include <cstring>
#include <iostream>

StreamingWhisperEngine::StreamingWhisperEngine(whisper_context* shared_ctx)
    : ctx_(shared_ctx),
      state_(nullptr),
      language_("es"),
      n_threads_(4),
      beam_size_(5),
      max_buffer_samples_(16000 * 30) {

    if (!ctx_) {
        throw std::runtime_error("[StreamingWhisperEngine] Null whisper context");
    }

    // Create a per-session state (lightweight, ~MB)
    state_ = whisper_init_state(ctx_);
    if (!state_) {
        throw std::runtime_error("[StreamingWhisperEngine] Failed to create whisper state");
    }

    // Pre-reserve buffer (30 seconds @ 16kHz)
    audio_buffer_.reserve(max_buffer_samples_);

    std::cout << "[StreamingWhisperEngine] Session state created" << std::endl;
}

StreamingWhisperEngine::~StreamingWhisperEngine() {
    if (state_) {
        whisper_free_state(state_);
        state_ = nullptr;
    }
    // ctx_ is NOT freed here — owned by ModelCache
}

void StreamingWhisperEngine::processAudioChunk(const std::vector<float>& pcm_data) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    size_t new_total_size = audio_buffer_.size() + pcm_data.size();
    
    if (new_total_size > static_cast<size_t>(max_buffer_samples_)) {
        if (pcm_data.size() >= static_cast<size_t>(max_buffer_samples_)) {
            audio_buffer_.clear();
            audio_buffer_.insert(audio_buffer_.end(), 
                               pcm_data.end() - max_buffer_samples_, 
                               pcm_data.end());
        } else {
            size_t to_discard = new_total_size - max_buffer_samples_;
            audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + to_discard);
            audio_buffer_.insert(audio_buffer_.end(), pcm_data.begin(), pcm_data.end());
        }
    } else {
        audio_buffer_.insert(audio_buffer_.end(), pcm_data.begin(), pcm_data.end());
    }
}

std::string StreamingWhisperEngine::transcribe() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    if (audio_buffer_.empty()) {
        return "";
    }
    
    // Use beam search for better quality
    whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.beam_search.beam_size = beam_size_;

    params.language        = language_.c_str();
    params.n_threads       = n_threads_;
    params.print_progress  = false;
    params.print_timestamps = false;
    params.print_realtime  = false;
    params.print_special   = false;
    params.translate       = false;
    params.single_segment  = false;

    // Quality tuning
    params.no_context      = false;   // Use previous transcription context
    params.suppress_blank  = true;    // Suppress blank tokens
    params.suppress_nst    = true;    // Suppress non-speech tokens

    // Temperature: start deterministic, fall back with increment
    params.temperature     = 0.0f;
    params.temperature_inc = 0.2f;
    params.entropy_thold   = 2.4f;
    params.logprob_thold   = -1.0f;
    params.no_speech_thold = 0.6f;

    // Initial prompt for domain guidance
    if (!initial_prompt_.empty()) {
        params.initial_prompt = initial_prompt_.c_str();
    }

    // Use per-session state for thread safety
    int result = whisper_full_with_state(
        ctx_, state_, params,
        audio_buffer_.data(),
        static_cast<int>(audio_buffer_.size())
    );
    
    if (result != 0) {
        throw std::runtime_error("Whisper transcription failed with code: " + std::to_string(result));
    }
    
    // Extract transcribed text from state
    std::string transcription;
    const int n_segments = whisper_full_n_segments_from_state(state_);
    
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(state_, i);
        if (text) {
            transcription += text;
        }
    }
    
    return transcription;
}

void StreamingWhisperEngine::reset() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    audio_buffer_.clear();
}

size_t StreamingWhisperEngine::getBufferSize() const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    return audio_buffer_.size();
}

void StreamingWhisperEngine::setLanguage(const std::string& lang) {
    language_ = lang;
}

void StreamingWhisperEngine::setThreads(int n_threads) {
    if (n_threads > 0) {
        n_threads_ = n_threads;
    }
}

void StreamingWhisperEngine::setBeamSize(int beam_size) {
    if (beam_size > 0) {
        beam_size_ = beam_size;
    }
}

void StreamingWhisperEngine::setInitialPrompt(const std::string& prompt) {
    initial_prompt_ = prompt;
}

bool StreamingWhisperEngine::isReady() const {
    return ctx_ != nullptr && state_ != nullptr;
}

std::vector<float> StreamingWhisperEngine::convertInt16ToFloat32(const std::vector<int16_t>& pcm16) {
    std::vector<float> pcm32(pcm16.size());
    for (size_t i = 0; i < pcm16.size(); ++i) {
        pcm32[i] = static_cast<float>(pcm16[i]) / 32768.0f;
    }
    return pcm32;
}

std::vector<float> StreamingWhisperEngine::convertBytesToFloat32(const std::vector<uint8_t>& bytes) {
    std::vector<int16_t> pcm16(bytes.size() / 2);
    std::memcpy(pcm16.data(), bytes.data(), bytes.size());
    return convertInt16ToFloat32(pcm16);
}
