#include "StreamingWhisperEngine.h"
#include <whisper.h>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <algorithm>
#include "InferenceLimiter.h"
#include "log/Log.h"
#include "utils/AudioPreprocessor.h"

StreamingWhisperEngine::StreamingWhisperEngine(whisper_context* shared_ctx)
    : ctx_(shared_ctx),
      state_(nullptr),
      language_("es"),
      n_threads_(4),
      beam_size_(5),
      vad_thold_(0.0f),
      temperature_(0.2f),
      temperature_inc_(0.2f),
      no_speech_thold_(0.3f),
      logprob_thold_(-1.0f),
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

    Log::info("Session state created");
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
    
    std::vector<float> prepped_data = pcm_data;
    AudioPreprocessor::process(prepped_data, hp_prev_raw_, hp_prev_filtered_);

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

StreamingWhisperEngine::TranscribeResult StreamingWhisperEngine::transcribeSlidingWindow(bool force_commit) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    TranscribeResult res;
    if (audio_buffer_.empty()) {
        return res;
    }
    
    // Use beam search when beam_size > 1, greedy otherwise
    whisper_full_params params = (beam_size_ > 1)
        ? whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH)
        : whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    params.language         = language_.c_str();
    params.n_threads        = n_threads_;
    params.print_progress   = false;
    params.print_timestamps = false;
    params.print_realtime   = false;
    params.print_special    = false;
    params.translate        = false;
    params.single_segment   = false;

    // Each window must be independent to avoid hallucinated context contaminating next window
    params.no_context       = true;
    params.suppress_blank   = true;
    params.suppress_nst     = true;

    // Apply beam size (was stored but never used before)
    if (beam_size_ > 1) {
        params.beam_search.beam_size = beam_size_;
    }

    params.temperature      = temperature_;
    params.temperature_inc  = temperature_inc_;
    params.no_speech_thold  = no_speech_thold_;
    params.logprob_thold    = logprob_thold_;

    // Limit encoder cross-attention to the actual audio duration.
    // whisper default: audio_ctx=1500 (= 30s). Each token = 20ms → 50 tok/s.
    // For a 5s buffer this saves ~83% of encoder attention work over zero-padded silence.
    {
        int ctx = static_cast<int>(
            std::ceil(static_cast<float>(audio_buffer_.size()) / 16000.0f * 50.0f));
        params.audio_ctx = std::max(ctx, 64); // floor at 64 tokens (~1.3s)
    }

    if (vad_thold_ > 0.0f) {
        params.no_speech_thold = vad_thold_;
    }

    if (!initial_prompt_.empty()) {
        params.initial_prompt = initial_prompt_.c_str();
    }

    int result = -1;
    {
        InferenceLimiter::Guard limit_guard;
        result = whisper_full_with_state(
            ctx_, state_, params,
            audio_buffer_.data(),
            audio_buffer_.size()
        );
    }
    
    if (result != 0) {
        std::cerr << "[StreamingWhisperEngine] ERROR: Whisper result=" << result << std::endl;
        throw std::runtime_error("Whisper transcription failed with code: " + std::to_string(result));
    }
    
    if (language_ == "auto") {
        int id = whisper_full_lang_id_from_state(state_);
        const char* detected = whisper_lang_str(id);
        if (detected && std::string(detected) != "auto") {
            language_ = detected;
            Log::info("Auto-detected language locked to: " + language_);
        }
    }
    
    const int n_segments = whisper_full_n_segments_from_state(state_);
    
    // We limit max window to ~10 seconds to keep inference time < 100ms
    const size_t max_window_samples = 16000 * 10;
    
    if (force_commit || audio_buffer_.size() >= max_window_samples) {
        int commit_up_to_segment = -1;
        int64_t commit_t1 = 0;
        
        if (force_commit) {
            commit_up_to_segment = n_segments - 1;
        } else {
            // Find the last segment that ends before the final 2 seconds of audio
            // Whisper timestamps are in 10ms (100 = 1 sec).
            int64_t overlap_bounds_t = (static_cast<int64_t>(audio_buffer_.size()) - 32000) / 160;
            
            for (int i = n_segments - 1; i >= 0; --i) {
                int64_t t1 = whisper_full_get_segment_t1_from_state(state_, i);
                if (t1 < overlap_bounds_t) {
                    commit_up_to_segment = i;
                    commit_t1 = t1;
                    break;
                }
            }
            
            // If the user spoke a continuous 10s sentence without any punctuation break,
            // we forcefully commit everything except the very last segment.
            if (commit_up_to_segment == -1 && n_segments > 1) {
                commit_up_to_segment = n_segments - 2;
                commit_t1 = whisper_full_get_segment_t1_from_state(state_, commit_up_to_segment);
            }
        }
        
        if (commit_up_to_segment >= 0) {
            for (int i = 0; i <= commit_up_to_segment; ++i) {
                const char* text = whisper_full_get_segment_text_from_state(state_, i);
                if (text) res.committed_text += text;
            }
            for (int i = commit_up_to_segment + 1; i < n_segments; ++i) {
                const char* text = whisper_full_get_segment_text_from_state(state_, i);
                if (text) res.partial_text += text;
            }
            
            // Shift audio buffer, dropping the committed audio to prevent duplicate transcriptions
            if (commit_t1 > 0) {
                size_t samples_to_erase = commit_t1 * 160;
                Log::debug("Committing " + std::to_string(samples_to_erase) + " samples: '" + res.committed_text + "'");
                if (samples_to_erase < audio_buffer_.size()) {
                    audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + samples_to_erase);
                } else {
                    audio_buffer_.clear();
                }
            } else if (force_commit) {
                Log::debug("Force commit, clearing buffer: '" + res.committed_text + "'");
                audio_buffer_.clear();
            }
            
            return res;
        }
    }
    
    // If not committing, all text is partial
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text_from_state(state_, i);
        if (text) res.partial_text += text;
    }
    
    Log::debug("Partial (n_seg=" + std::to_string(n_segments) + "): '" + res.partial_text + "'");
    
    return res;
}

std::string StreamingWhisperEngine::transcribe(size_t start_offset) {
    // Legacy mapping (ignores start_offset which is no longer used outside of testing)
    return transcribeSlidingWindow(true).committed_text;
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

void StreamingWhisperEngine::setTemperature(float temperature) {
    temperature_ = temperature;
}

void StreamingWhisperEngine::setTemperatureInc(float temperature_inc) {
    temperature_inc_ = temperature_inc;
}

void StreamingWhisperEngine::setNoSpeechThreshold(float no_speech_thold) {
    no_speech_thold_ = no_speech_thold;
}

void StreamingWhisperEngine::setLogprobThreshold(float logprob_thold) {
    logprob_thold_ = logprob_thold;
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
