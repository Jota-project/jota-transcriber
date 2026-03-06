#include "StreamingWhisperEngine.h"
#include <whisper.h>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <cmath>
#include <algorithm>
#include "InferenceLimiter.h"

StreamingWhisperEngine::StreamingWhisperEngine(whisper_context* shared_ctx)
    : ctx_(shared_ctx),
      state_(nullptr),
      language_("es"),
      n_threads_(4),
      beam_size_(5),
      vad_thold_(0.0f),
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
    
    // Low-pass/High-pass filter variables (IIR)
    // Filter out < 80Hz rumble (alpha ~ 0.97 at 16kHz)
    const float alpha = 0.969f;
    static float prev_raw = 0.0f;
    static float prev_filtered = 0.0f;
    
    // Apply High-pass filter, track peak for normalization
    float peak = 0.0f;
    std::vector<float> prepped_data = pcm_data; // copy
    for (size_t i = 0; i < prepped_data.size(); ++i) {
        // High pass filter
        float raw = prepped_data[i];
        float filtered = alpha * (prev_filtered + raw - prev_raw);
        prev_raw = raw;
        prev_filtered = filtered;
        
        prepped_data[i] = filtered;
        peak = std::max(peak, std::abs(filtered));
    }
    
    // Fast Peak Normalization ~ 0.9 if it's too quiet, but only if there is *some* signal
    if (peak > 0.0001f && peak < 0.9f) {
        float gain = 0.9f / peak;
        // Don't boost static too much
        gain = std::min(gain, 10.0f);
        for (float& sample : prepped_data) {
            sample *= gain;
        }
    }

    size_t new_total_size = audio_buffer_.size() + prepped_data.size();
    
    if (new_total_size > static_cast<size_t>(max_buffer_samples_)) {
        if (prepped_data.size() >= static_cast<size_t>(max_buffer_samples_)) {
            audio_buffer_.clear();
            audio_buffer_.insert(audio_buffer_.end(), 
                               prepped_data.end() - max_buffer_samples_, 
                               prepped_data.end());
        } else {
            size_t to_discard = new_total_size - max_buffer_samples_;
            audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + to_discard);
            audio_buffer_.insert(audio_buffer_.end(), prepped_data.begin(), prepped_data.end());
        }
    } else {
        audio_buffer_.insert(audio_buffer_.end(), prepped_data.begin(), prepped_data.end());
    }
}

std::string StreamingWhisperEngine::transcribe(size_t start_offset) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    if (audio_buffer_.empty() || start_offset >= audio_buffer_.size()) {
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

    // VAD
    if (vad_thold_ > 0.0f) {
        params.audio_ctx = 0; // Use default
        // VAD parameters in whisper_full_params (whisper.cpp specific)
        // Set no_speech threshold more strictly if VAD is manually requested
        params.no_speech_thold = vad_thold_;
    }

    // Initial prompt for domain guidance
    if (!initial_prompt_.empty()) {
        params.initial_prompt = initial_prompt_.c_str();
    }

    // Use per-session state for thread safety, only process from start_offset
    const float* data_ptr = audio_buffer_.data() + start_offset;
    int data_size = static_cast<int>(audio_buffer_.size() - start_offset);
    
    int result = -1;
    {
        // Global Concurrency Limit: blocks if too many inferences are running
        InferenceLimiter::Guard limit_guard;
        
        result = whisper_full_with_state(
            ctx_, state_, params,
            data_ptr,
            data_size
        );
    }
    
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

void StreamingWhisperEngine::reset(size_t keep_samples) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (keep_samples == 0 || keep_samples >= audio_buffer_.size()) {
        audio_buffer_.clear();
    } else {
        audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.end() - keep_samples);
    }
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

void StreamingWhisperEngine::setVadThreshold(float vad_thold) {
    vad_thold_ = vad_thold;
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
