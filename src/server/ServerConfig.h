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

    // Auth API
    std::string auth_api_url;           // empty = auth disabled
    std::string auth_api_secret;        // Authorization: Bearer <...>
    int auth_cache_ttl = 300;           // seconds
    int auth_api_timeout = 5;           // seconds

    // MQTT
    std::string mqtt_url;               // empty = MQTT disabled (e.g. mqtt://localhost:1883)
    std::string mqtt_topic       = "transcription";
    std::string mqtt_client_id   = "transcription_server";

    // Whisper quality tuning
    int whisper_beam_size = 5;          // beam search size (1 = greedy)
    int whisper_threads = 4;            // threads per transcription
    int model_cache_ttl = 300;          // seconds to keep model after last session (0 = immediate, -1 = forever)
    std::string whisper_initial_prompt; // optional initial prompt for decoder guidance
};
