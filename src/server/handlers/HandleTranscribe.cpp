#include "HandleTranscribe.h"
#include "server/ServerConfig.h"
#include "server/AuthManager.h"
#include "server/MultipartParser.h"
#include "audio/AudioDecoder.h"
#include "whisper/ModelCache.h"
#include "whisper/InferenceLimiter.h"
#include "log/Log.h"
#include <whisper.h>
#include <nlohmann/json.hpp>
#include <boost/beast/version.hpp>

using json = nlohmann::json;
namespace http = boost::beast::http;

namespace {

http::response<http::string_body> makeJson(http::status status,
                                           const std::string& body,
                                           unsigned version) {
    http::response<http::string_body> res;
    res.version(version);
    res.result(status);
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "application/json");
    res.body() = body;
    res.prepare_payload();
    return res;
}

http::response<http::string_body> makeError(http::status status,
                                             const std::string& message,
                                             const std::string& type,
                                             unsigned version) {
    json err = {{"error", {{"message", message}, {"type", type}}}};
    return makeJson(status, err.dump(), version);
}

} // namespace

void HandleTranscribe::handle(const http::request<http::string_body>& req,
                              SendFn send,
                              const ServerConfig& config,
                              const std::shared_ptr<AuthManager>& auth) {
    const unsigned ver = req.version();

    // ── 1. Auth ───────────────────────────────────────────────────────────────
    if (auth && auth->isAuthEnabled()) {
        std::string auth_header = std::string(req[http::field::authorization]);
        std::string token;
        if (auth_header.rfind("Bearer ", 0) == 0)
            token = auth_header.substr(7);
        if (!auth->validate(token)) {
            send(makeError(http::status::unauthorized,
                           "Invalid or missing authorization token",
                           "invalid_request_error", ver));
            return;
        }
    }

    // ── 2. Size limit ─────────────────────────────────────────────────────────
    if (req.body().size() > config.max_upload_bytes) {
        send(makeError(http::status::payload_too_large,
                       "Request body exceeds maximum allowed size",
                       "invalid_request_error", ver));
        return;
    }

    // ── 3. Parse multipart ────────────────────────────────────────────────────
    std::string content_type = std::string(req[http::field::content_type]);
    std::string boundary = MultipartParser::extractBoundary(content_type);
    if (boundary.empty()) {
        send(makeError(http::status::bad_request,
                       "Content-Type must be multipart/form-data with a boundary",
                       "invalid_request_error", ver));
        return;
    }

    std::unordered_map<std::string, MultipartParser::Part> parts;
    try {
        parts = MultipartParser::parse(req.body(), boundary);
    } catch (const std::exception& e) {
        send(makeError(http::status::bad_request,
                       std::string("Failed to parse multipart body: ") + e.what(),
                       "invalid_request_error", ver));
        return;
    }

    if (!parts.count("file")) {
        send(makeError(http::status::bad_request,
                       "Missing required field: file",
                       "invalid_request_error", ver));
        return;
    }
    if (!parts.count("model")) {
        send(makeError(http::status::bad_request,
                       "Missing required field: model",
                       "invalid_request_error", ver));
        return;
    }

    // ── 4. Decode audio ───────────────────────────────────────────────────────
    std::vector<float> pcm;
    try {
        pcm = AudioDecoder::decode(parts.at("file").data);
    } catch (const std::exception& e) {
        send(makeError(http::status::bad_request,
                       std::string("Failed to decode audio: ") + e.what(),
                       "invalid_request_error", ver));
        return;
    }

    // ── 5. Inference capacity check ───────────────────────────────────────────
    if (!InferenceLimiter::instance().hasCapacity()) {
        send(makeError(http::status::service_unavailable,
                       "Server is at maximum inference capacity, try again later",
                       "server_error", ver));
        return;
    }

    // ── 6. Acquire model context ──────────────────────────────────────────────
    whisper_context* ctx = nullptr;
    try {
        ctx = ModelCache::instance().acquire(config.model_path);
    } catch (const std::exception& e) {
        Log::error(std::string("HandleTranscribe: model load failed: ") + e.what());
        send(makeError(http::status::internal_server_error,
                       "Failed to load model", "server_error", ver));
        return;
    }

    // ── 7. Transcribe ─────────────────────────────────────────────────────────
    std::string text;
    {
        InferenceLimiter::Guard guard;

        whisper_state* state = whisper_init_state(ctx);
        if (!state) {
            ModelCache::instance().release();
            send(makeError(http::status::internal_server_error,
                           "Failed to initialize whisper state", "server_error", ver));
            return;
        }

        // Determine language
        std::string language = "auto";
        if (parts.count("language")) {
            language.assign(parts.at("language").data.begin(),
                            parts.at("language").data.end());
        }

        whisper_full_params params = (config.whisper_beam_size > 1)
            ? whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH)
            : whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

        params.language         = language.c_str();
        params.n_threads        = config.whisper_threads;
        params.print_progress   = false;
        params.print_timestamps = false;
        params.print_realtime   = false;
        params.print_special    = false;
        params.translate        = false;
        params.no_context       = false; // context helps for full-file transcription
        params.suppress_blank   = true;
        params.suppress_nst     = true;
        params.temperature      = config.whisper_temperature;
        params.temperature_inc  = config.whisper_temperature_inc;
        params.no_speech_thold  = config.whisper_no_speech_thold;
        params.logprob_thold    = config.whisper_logprob_thold;

        if (config.whisper_beam_size > 1)
            params.beam_search.beam_size = config.whisper_beam_size;

        // Request-level overrides
        std::string prompt;
        if (parts.count("prompt")) {
            prompt.assign(parts.at("prompt").data.begin(), parts.at("prompt").data.end());
            params.initial_prompt = prompt.c_str();
        } else if (!config.whisper_initial_prompt.empty()) {
            params.initial_prompt = config.whisper_initial_prompt.c_str();
        }

        if (parts.count("temperature")) {
            try {
                std::string t(parts.at("temperature").data.begin(),
                              parts.at("temperature").data.end());
                params.temperature = std::stof(t);
            } catch (...) { /* ignore invalid value, use default */ }
        }

        int rc = whisper_full_with_state(
            ctx, state, params,
            pcm.data(), static_cast<int>(pcm.size()));

        if (rc != 0) {
            whisper_free_state(state);
            ModelCache::instance().release();
            Log::error("HandleTranscribe: whisper returned " + std::to_string(rc));
            send(makeError(http::status::internal_server_error,
                           "Whisper inference failed", "server_error", ver));
            return;
        }

        int n = whisper_full_n_segments_from_state(state);
        for (int i = 0; i < n; ++i) {
            const char* seg = whisper_full_get_segment_text_from_state(state, i);
            if (seg) text += seg;
        }

        whisper_free_state(state);
    }

    ModelCache::instance().release();

    Log::info("HandleTranscribe: transcribed " + std::to_string(pcm.size()) +
              " samples → " + std::to_string(text.size()) + " chars");

    json response = {{"text", text}};
    send(makeJson(http::status::ok, response.dump(), ver));
}