#pragma once
#include <string>

struct ServerConfig {
    std::string model_path = "third_party/whisper.cpp/models/ggml-small.bin";
    std::string bind_address = "0.0.0.0";
    unsigned short port = 9001;
    std::string cert_path;
    std::string key_path;
    size_t max_connections = 8;
    size_t max_connections_per_ip = 2;
    std::string trusted_proxy_hosts;    // CSV hostnames exempt from per-IP limit (empty = off)
    int trusted_proxy_refresh_sec = 30; // DNS re-resolution interval for trusted hosts
    int session_timeout_sec = 30;       // seconds before disconnecting idle sessions

    // Auth
    std::string auth_token;             // static token (simple deployments, no API needed)
    std::string auth_api_url;           // external auth API (takes precedence over auth_token)
    std::string auth_api_secret;        // Authorization: Bearer <...>
    int auth_cache_ttl = 300;           // seconds
    int auth_api_timeout = 5;           // seconds

    int whisper_beam_size = 1;          // beam search size (1 = greedy, fastest for streaming)
    int whisper_threads = 4;            // threads per transcription
    int max_concurrent_inference = 4;   // Max simultaneous whisper decodes
    int model_cache_ttl = 300;          // seconds to keep model after last session (0 = immediate, -1 = forever)
    std::string whisper_initial_prompt; // optional initial prompt for decoder guidance

    // Whisper inference quality/speed tuning
    float whisper_temperature = 0.0f;       // initial sampling temperature (0.0=greedy, fastest for streaming)
    float whisper_temperature_inc = 0.0f;   // temperature increment on repetition (0.0 disables fallback)
    float whisper_no_speech_thold = 0.3f;   // probability threshold to reject non-speech segments
    float whisper_logprob_thold = -0.7f;    // log-prob threshold to reject low-confidence segments (-1.0=disabled)
    int flush_min_new_audio_ms = 500;       // ms of new audio required before flushLoop re-runs inference (was hardcoded 250ms)

    int shutdown_timeout_sec = 10;      // max seconds to wait for sessions to close on SIGINT/SIGTERM
    size_t max_upload_bytes = 25 * 1024 * 1024; // 25 MB — matches OpenAI limit
};
