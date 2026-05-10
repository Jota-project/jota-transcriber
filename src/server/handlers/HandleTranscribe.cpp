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
#include <unordered_set>
#include <sstream>
#include <iomanip>
#include <ctime>

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

    // ── 2. Parse multipart ────────────────────────────────────────────────────
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

    // ── 3. Check body size ────────────────────────────────────────────────────
    if (req.body().size() > config.max_upload_bytes) {
        send(makeError(http::status::payload_too_large,
                       "Request body exceeds maximum upload size",
                       "payload_too_large_error", ver));
        return;
    }

    // ── 3b. response_format ───────────────────────────────────────────────────
    std::string response_format = "json";
    if (parts.count("response_format")) {
        response_format.assign(parts.at("response_format").data.begin(),
                               parts.at("response_format").data.end());
    }
    static const std::unordered_set<std::string> kSupportedFormats{"json", "text", "verbose_json"};
    if (!kSupportedFormats.count(response_format)) {
        send(makeError(http::status::bad_request,
                       "response_format '" + response_format + "' is not supported. "
                       "Supported values: json, text, verbose_json",
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
    // Declared before guard so they're accessible when formatting the response below.
    struct Segment {
        float       start;
        float       end;
        std::string text;
        float       no_speech_prob;
    };
    std::vector<Segment> segments;

    std::string language = "auto";
    if (parts.count("language")) {
        language.assign(parts.at("language").data.begin(),
                        parts.at("language").data.end());
    }

    std::string task = "transcribe";
    if (parts.count("task")) {
        task.assign(parts.at("task").data.begin(),
                    parts.at("task").data.end());
    }

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

        // Determine language (already extracted above)
        whisper_full_params params = (config.whisper_beam_size > 1)
            ? whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH)
            : whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

        params.language         = language.c_str();
        params.n_threads        = config.whisper_threads;
        params.print_progress   = false;
        params.print_timestamps = false;
        params.print_realtime   = false;
        params.print_special    = false;
        params.translate        = (task == "translate");
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
                float val = std::stof(t);
                if (val < 0.0f || val > 1.0f) {
                    whisper_free_state(state);
                    ModelCache::instance().release();
                    send(makeError(http::status::bad_request,
                                   "temperature must be between 0.0 and 1.0",
                                   "invalid_request_error", ver));
                    return;
                }
                params.temperature = val;
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
            if (!seg) continue;
            if (response_format == "verbose_json") {
                int64_t t0  = whisper_full_get_segment_t0_from_state(state, i);
                int64_t t1  = whisper_full_get_segment_t1_from_state(state, i);
                float   nsp = whisper_full_get_segment_no_speech_prob_from_state(state, i);
                segments.push_back({t0 / 100.0f, t1 / 100.0f, seg, nsp});
            }
            text += seg;
        }

        whisper_free_state(state);
    }

    ModelCache::instance().release();
    Log::info("HandleTranscribe: transcribed " + std::to_string(pcm.size()) +
            " samples → " + std::to_string(text.size()) + " chars"
                + " format=" + response_format);

    if (response_format == "text") {
        auto first = text.find_first_not_of(" \t\r\n");
        std::string trimmed = (first == std::string::npos) ? "" : text.substr(first);
        http::response<http::string_body> res;
        res.version(ver);
        res.result(http::status::ok);
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/plain; charset=utf-8");
        res.body() = trimmed;
        res.prepare_payload();
        send(res);
        return;
    }

    if (response_format == "json") {
        send(makeJson(http::status::ok, json{{"text", text}}.dump(), ver));
        return;
    }

    // verbose_json
    json segs = json::array();
    for (const auto& s : segments) {
        segs.push_back({
            {"start",          s.start},
            {"end",            s.end},
            {"text",           s.text},
            {"no_speech_prob", s.no_speech_prob}
        });
    }

    // Generate id and timestamp
    std::stringstream ss;
    ss << "transcribe-" << std::hex << std::setw(16) << std::setfill('0') << rand();
    std::string id = ss.str();

    std::time_t now = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));


    json vj = {
        {"id",         id},
        {"created_at",  std::string(buf)},
        {"task",       task},
        {"language",   language},
        {"duration",   segments.empty() ? 0.0f : segments.back().end},
        {"text",       text},
        {"segments",   segs}
    };
    send(makeJson(http::status::ok, vj.dump(), ver));
}