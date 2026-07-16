#pragma once
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <iostream>
#include <cfloat>
#include "whisper/StreamingWhisperEngine.h"
#include "whisper/ModelCache.h"
#include "server/SessionTracker.h"
#include "AuthManager.h"
#include "ConnectionGuard.h"
#include "log/Log.h"
#include "utils/HallucinationGuard.h"
#include "whisper/InferenceLimiter.h"
#include "server/SocketUtil.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

// ms of audio @16kHz mono float32 → sample count.
inline size_t msToSamples16kHz(int ms) {
    return static_cast<size_t>(ms) * 16;
}

template <class StreamType>
class StreamingSession : public std::enable_shared_from_this<StreamingSession<StreamType>>, public SessionTracker::SessionBase {
public:
    StreamingSession(
        StreamType&& ws,
        const std::string& model_path,
        std::shared_ptr<AuthManager> auth_manager,
        int whisper_beam_size = 5,
        int whisper_threads = 4,
        const std::string& whisper_initial_prompt = "",
        int session_timeout_sec = 30,
        float whisper_temperature = 0.2f,
        float whisper_temperature_inc = 0.2f,
        float whisper_no_speech_thold = 0.3f,
        float whisper_logprob_thold = -1.0f,
        int flush_min_new_audio_ms = 500,
        const std::string& vad_model_path = "",
        float vad_threshold = 0.5f,
        int vad_min_speech_ms = 250,
        int vad_min_silence_ms = 2000,
        float vad_max_speech_s = FLT_MAX,
        int vad_speech_pad_ms = 400,
        float vad_samples_overlap = 0.1f
    )
        : ws_(std::move(ws)),
          model_path_(model_path),
          auth_manager_(auth_manager),
          configured_(false),
          buffer_overflowed_(false),
          dropped_chunks_(0),
          capacity_degraded_(false),
          last_transcribed_size_(0),
          language_("es"),
          whisper_beam_size_(whisper_beam_size),
          whisper_threads_(whisper_threads),
          whisper_initial_prompt_(whisper_initial_prompt),
          session_timeout_sec_(session_timeout_sec),
          whisper_temperature_(whisper_temperature),
          whisper_temperature_inc_(whisper_temperature_inc),
          whisper_no_speech_thold_(whisper_no_speech_thold),
          whisper_logprob_thold_(whisper_logprob_thold),
          flush_min_new_audio_ms_(flush_min_new_audio_ms),
          vad_model_path_(vad_model_path),
          vad_threshold_(vad_threshold),
          vad_min_speech_ms_(vad_min_speech_ms),
          vad_min_silence_ms_(vad_min_silence_ms),
          vad_max_speech_s_(vad_max_speech_s),
          vad_speech_pad_ms_(vad_speech_pad_ms),
          vad_samples_overlap_(vad_samples_overlap),
          model_acquired_(false),
          bytes_received_in_window_(0),
          rate_limit_start_(std::chrono::steady_clock::now()),
          flush_running_(false),
          last_audio_time_(std::chrono::steady_clock::now())
    {
        session_id_ = generateSessionId();
        Log::info("Session created", session_id_);
        SessionTracker::instance().add(this);
    }

    ~StreamingSession() override {
        stopFlushLoop();
        SessionTracker::instance().remove(this);
        releaseModel();
    }

    void shutdown() override {
        // Triggered asynchronously by signal handler (SessionTracker::shutdownAll()),
        // from a thread other than the one possibly blocked in ws_.read() below.
        // .cancel() alone only cancels pending *async* operations — it does not
        // reliably interrupt a synchronous read already in progress (same gotcha
        // as the idle-watchdog in flushLoop(), see jota-transcriber#68). Force-shut
        // the native socket too, so a blocked read unblocks with an error either way.
        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).cancel(ec);
        forceSocketShutdown(beast::get_lowest_layer(ws_).native_handle());
    }

    template<class Req>
    void run(const Req& req) {
        try {
            // Idle timeout is enforced by flushLoop()'s watchdog (see below), not by
            // SO_RCVTIMEO: that socket option does not reliably interrupt a
            // synchronous Boost.Asio/Beast read (jota-transcriber#68) — Asio treats
            // the resulting EWOULDBLOCK as a spurious wakeup and retries the
            // blocking read instead of propagating a timeout.
            ws_.accept(req);
            Log::info("WebSocket handshake accepted", session_id_);

            flush_running_ = true;
            flush_thread_ = std::thread([this]() { this->flushLoop(); });

            while (true) {
                beast::flat_buffer buffer;
                ws_.read(buffer);

                if (ws_.got_text()) {
                    std::string message(
                        boost::asio::buffers_begin(buffer.data()),
                        boost::asio::buffers_end(buffer.data())
                    );
                    handleJsonMessage(message);
                } else {
                    std::vector<unsigned char> data(buffer.size());
                    boost::asio::buffer_copy(boost::asio::buffer(data), buffer.data());
                    handleBinaryMessage(data);
                }
            }
        }
        catch (beast::system_error const& se) {
            if (se.code() == websocket::error::closed) {
                Log::info("Session closed by client", session_id_);
            } else {
                Log::error("Read error [" + std::to_string(se.code().value()) + "]: " + se.code().message(), session_id_);
            }
        }
        catch (std::exception const& e) {
            Log::error(std::string("Unexpected exception: ") + e.what(), session_id_);
        }

        releaseModel();
    }

private:
    void releaseModel() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (model_acquired_) {
            engine_.reset();
            ModelCache::instance().release();
            model_acquired_ = false;
            Log::info("Model reference released", session_id_);
        }
    }

    void sendMessage(const json& msg) {
        try {
            std::string str = msg.dump();
            std::lock_guard<std::mutex> lock(write_mutex_);
            ws_.text(true);
            ws_.write(net::buffer(str));
        }
        catch (std::exception& e) {
            Log::error(std::string("Failed to send message: ") + e.what(), session_id_);
        }
    }

    void sendError(const std::string& message, const std::string& code) {
        json msg = {
            {"type", "error"},
            {"message", message},
            {"code", code}
        };
        sendMessage(msg);
    }

    void sendReady() {
        json msg = {
            {"type", "ready"},
            {"protocol_version", 1},
            {"session_id", session_id_},
            {"config", {
                {"language", language_},
                {"sample_rate", 16000},
                {"beam_size", whisper_beam_size_}
            }}
        };
        sendMessage(msg);
    }

    void processAudioChunk(const std::vector<float>& audio) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        if (!configured_ || !engine_) return;

        last_audio_time_ = std::chrono::steady_clock::now();
        bool overflow = engine_->processAudioChunk(audio);
        bool entering_episode = overflow && !buffer_overflowed_;
        size_t current_dropped = 0;
        if (overflow) {
            buffer_overflowed_ = true;
            current_dropped = ++dropped_chunks_;
        }
        lock.unlock();

        if (entering_episode) {
            Log::warn("Audio buffer full, entrando en pausa de flujo", session_id_);
            sendMessage({{"type", "flow_control"}, {"action", "pause"}});
        }
        constexpr size_t WARNING_INTERVAL_CHUNKS = 10;
        if (overflow && current_dropped % WARNING_INTERVAL_CHUNKS == 0) {
            sendMessage({
                {"type", "warning"},
                {"code", "buffer_full"},
                {"dropped_chunks", current_dropped},
                {"message", "Audio buffer full, dropping incoming audio"}
            });
        }
        // Inference is handled entirely by flushLoop to avoid blocking the receive loop.
    }

    void handleJsonMessage(const std::string& message) {
        try {
            json msg = json::parse(message);

            if (!msg.contains("type")) {
                Log::warn("Received JSON without 'type' field", session_id_);
                sendError("Missing 'type' field", "INVALID_MESSAGE");
                return;
            }

            std::string type = msg["type"];
            Log::debug("JSON message received: type=" + type, session_id_);

            if (type == "config") {
                handleConfig(msg);
            }
            else if (!configured_) {
                Log::warn("Message type='" + type + "' received before config", session_id_);
                sendError("Session not configured. Send 'config' first.", "NOT_CONFIGURED");
            }
            else if (type == "end") {
                handleEnd();
            }
            else {
                Log::warn("Unknown message type: " + type, session_id_);
                sendError("Unknown message type: " + type, "UNKNOWN_TYPE");
            }
        }
        catch (json::parse_error& e) {
            Log::warn(std::string("JSON parse error: ") + e.what(), session_id_);
            sendError("Invalid JSON: " + std::string(e.what()), "PARSE_ERROR");
        }
    }

    void handleBinaryMessage(const std::vector<unsigned char>& data) {
        // Guard: reject oversized frames immediately — 1 MB = ~16s of float32 audio @ 16kHz,
        // far beyond any legitimate streaming chunk.
        constexpr size_t MAX_FRAME_BYTES = 1 * 1024 * 1024; // 1 MB
        if (data.size() > MAX_FRAME_BYTES) {
            Log::warn("Binary frame too large (" + std::to_string(data.size()) +
                      " bytes > 1MB limit), closing connection", session_id_);
            boost::system::error_code ec;
            ws_.close(websocket::close_reason(websocket::close_code::policy_error, "Frame too large"), ec);
            return;
        }

        if (data.size() < sizeof(float)) {
            Log::warn("Binary frame too small for float32 (" + std::to_string(data.size()) + " bytes), ignoring", session_id_);
            return;
        }

        if (!configured_) {
            Log::warn("Binary frame received before config (" + std::to_string(data.size()) + " bytes)", session_id_);
            sendError("Session not configured. Send 'config' first.", "NOT_CONFIGURED");
            return;
        }

        if (data.size() % sizeof(float) != 0) {
            Log::warn("Binary frame size not aligned to float32 (" + std::to_string(data.size()) + " bytes), ignoring", session_id_);
            return;
        }

        // Enforce Binary Rate Limit (QoS) — only count frames that pass all validation guards.
        auto now = std::chrono::steady_clock::now();
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - rate_limit_start_).count();

        bytes_received_in_window_ += data.size();

        if (elapsed_s >= 3) {
            // Fixed 3-second window: max 600 KB per window (~200 KB/s average).
            // Note: this is a fixed window, not sliding — resets every 3 seconds.
            const size_t MAX_BYTES_PER_WINDOW = 200 * 1024 * 3;
            if (bytes_received_in_window_ > MAX_BYTES_PER_WINDOW) {
                Log::warn("Rate limit exceeded (" + std::to_string(bytes_received_in_window_) +
                          " bytes in " + std::to_string(elapsed_s) + "s)", session_id_);
                boost::system::error_code ec;
                ws_.close(websocket::close_reason(websocket::close_code::policy_error, "Rate limit exceeded"), ec);
                return;
            }
            rate_limit_start_ = now;
            bytes_received_in_window_ = 0;
        }

        try {
            auto data_ptr = reinterpret_cast<const float*>(data.data());
            size_t size = data.size() / sizeof(float);
            std::vector<float> pcm(data_ptr, data_ptr + size);
            processAudioChunk(pcm);
        }
        catch (std::exception& e) {
            Log::error(std::string("Audio processing failed: ") + e.what(), session_id_);
            sendError("Binary audio processing failed: " + std::string(e.what()), "AUDIO_ERROR");
        }
    }

    void handleConfig(const json& msg) {
        Log::info("Config message received", session_id_);
        try {
            if (auth_manager_->isAuthEnabled()) {
                if (!msg.contains("token") || !msg["token"].is_string()) {
                    Log::warn("Auth failed: missing or invalid 'token' field", session_id_);
                    sendError("Missing or invalid 'token'", "AUTH_REQUIRED");
                    ws_.close(websocket::close_code::policy_error);
                    return;
                }

                std::string token = msg["token"];
                Log::debug("Validating token: " + Log::maskKey(token), session_id_);

                if (!auth_manager_->validate(token)) {
                    Log::warn("Auth failed: token rejected (key=" + Log::maskKey(token) + ")", session_id_);
                    sendError("Invalid token", "AUTH_FAILED");
                    ws_.close(websocket::close_code::policy_error);
                    return;
                }

                Log::info("Auth passed (key=" + Log::maskKey(token) + ")", session_id_);
            }

            if (msg.contains("language")) {
                language_ = msg["language"];
            }

            // VAD configure (0.0 = disabled, try a safe 0.4 for long silences only if enabled by client)
            float vad_thold = 0.0f;
            if (msg.contains("vad_thold") && msg["vad_thold"].is_number()) {
                vad_thold = msg["vad_thold"].get<float>();
            }

            // Acquire model from cache (loads if not already loaded, instant if cached)
            Log::info("Acquiring model from cache: " + model_path_, session_id_);
            whisper_context* ctx = ModelCache::instance().acquire(model_path_);
            
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                model_acquired_ = true;

                // Create engine with shared context (creates its own whisper_state)
                engine_ = std::make_unique<StreamingWhisperEngine>(ctx);
                engine_->setLanguage(language_);
                engine_->setThreads(whisper_threads_);
                engine_->setBeamSize(whisper_beam_size_);
                engine_->setVadThreshold(vad_thold);
                engine_->setTemperature(whisper_temperature_);
                engine_->setTemperatureInc(whisper_temperature_inc_);
                engine_->setNoSpeechThreshold(whisper_no_speech_thold_);
                engine_->setLogprobThreshold(whisper_logprob_thold_);
                if (!vad_model_path_.empty()) {
                    engine_->setVadConfig(vad_model_path_, vad_threshold_,
                                          vad_min_speech_ms_, vad_min_silence_ms_,
                                          vad_max_speech_s_, vad_speech_pad_ms_,
                                          vad_samples_overlap_);
                }
                if (!whisper_initial_prompt_.empty()) {
                    engine_->setInitialPrompt(whisper_initial_prompt_);
                }

                configured_ = true;
                last_transcribed_size_ = 0;
                capacity_degraded_ = false;
                full_transcription_    = "";
                raw_transcription_     = "";
                last_audio_time_ = std::chrono::steady_clock::now();
            }

            Log::info("Session ready (lang=" + language_ +
                      ", beam=" + std::to_string(whisper_beam_size_) +
                      ", vad=" + std::to_string(vad_thold) +
                      ")", session_id_);
            sendReady();
        }
        catch (std::exception& e) {
            Log::error(std::string("Config failed: ") + e.what(), session_id_);
            sendError("Configuration failed: " + std::string(e.what()), "CONFIG_ERROR");
        }
    }

    void handleEnd() {
        Log::info("End-of-stream received, running final transcription", session_id_);

        // Stop flushLoop BEFORE running final inference. This is the fix for the race
        // condition (Jota-project/jota-transcriber#27): without this join, flushLoop can
        // wake up and send another message between our sendMessage(is_final=true) and
        // ws_.close(), or race ws_.close() with a concurrent ws_.write().
        stopFlushLoop();

        std::string final_text;
        bool complete = true;
        std::string incomplete_reason;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (engine_) {
                // Bounded wait: don't let the final decode compete indefinitely for a
                // saturated GPU (jota-project/jota-transcriber#62). If no slot frees up
                // within the timeout, fall back to whatever was already committed instead
                // of blocking session close indefinitely.
                InferenceLimiter::TryGuard guard(std::chrono::milliseconds(3000));
                if (guard.acquired()) {
                    auto res = engine_->transcribeSlidingWindow(true); // force commit
                    // Note: no hallucination guard here — this is the last chance to capture audio
                    // that the engine still holds in its buffer.
                    full_transcription_ += res.committed_text;
                } else {
                    complete = false;
                    incomplete_reason = "gpu_saturated_timeout";
                    Log::warn("handleEnd: timeout esperando slot de inferencia, usando texto parcial/raw acumulado", session_id_);
                }
            }

            // Fallback: if all flushLoop commits were hallucination-filtered (audio was erased
            // from the engine buffer but text was discarded), full_transcription_ is empty.
            // Use the raw accumulated text so the client receives something rather than nothing.
            if (full_transcription_.empty() && !raw_transcription_.empty()) {
                Log::warn("full_transcription_ empty at end-of-session — using raw fallback (all commits were hallucination-filtered)", session_id_);
                full_transcription_ = raw_transcription_;
            }

            // Copy under the mutex so the read below is not a data race.
            final_text = full_transcription_;
        }

        Log::info("Final transcription: \"" + final_text + "\"", session_id_);
        json msg = {
            {"type", "transcription"},
            {"text", final_text},
            {"is_final", true}
        };
        if (!complete) {
            msg["complete"] = false;
            msg["reason"] = incomplete_reason;
        }
        sendMessage(msg);

        try {
            // ws_.close() must be serialized with ws_.write() via write_mutex_.
            // Beast does not support concurrent close + write on the same stream.
            std::lock_guard<std::mutex> lock(write_mutex_);
            ws_.close(websocket::close_code::normal);
            Log::info("Session closed normally", session_id_);
        }
        catch (std::exception& e) {
            Log::error(std::string("Error closing WebSocket: ") + e.what(), session_id_);
        }
    }


    std::string generateSessionId() {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

        static thread_local std::mt19937 gen(std::random_device{}());
        std::uniform_int_distribution<> dis(1000, 9999);

        std::ostringstream oss;
        oss << "session-" << timestamp << "-" << dis(gen);
        return oss.str();
    }

    // Member variables
    StreamType ws_;
    std::string model_path_;
    std::shared_ptr<AuthManager> auth_manager_;
    std::unique_ptr<StreamingWhisperEngine> engine_;
    std::string session_id_;
    bool configured_;
    bool buffer_overflowed_; // true while a saturation episode (HWM..LWM hysteresis) is active
    size_t dropped_chunks_;  // chunks dropped in the CURRENT episode; resets to 0 when it ends
    bool capacity_degraded_; // true while this session's own inference cycles are being skipped (GPU saturated)
    size_t last_transcribed_size_;
    std::string language_;

    // Whisper params
    int whisper_beam_size_;
    int whisper_threads_;
    std::string whisper_initial_prompt_;
    int session_timeout_sec_;
    float whisper_temperature_;
    float whisper_temperature_inc_;
    float whisper_no_speech_thold_;
    float whisper_logprob_thold_;
    int flush_min_new_audio_ms_;
    std::string vad_model_path_;
    float vad_threshold_;
    int   vad_min_speech_ms_;
    int   vad_min_silence_ms_;
    float vad_max_speech_s_;
    int   vad_speech_pad_ms_;
    float vad_samples_overlap_;
    bool model_acquired_;
    
    // Rate limiting & Timeout
    size_t bytes_received_in_window_;
    std::chrono::steady_clock::time_point rate_limit_start_;

    // Threading & Sync
    std::mutex write_mutex_;
    std::mutex state_mutex_;
    std::thread flush_thread_;
    std::atomic<bool> flush_running_;
    std::mutex flush_cv_mutex_;
    std::condition_variable flush_cv_;
    std::chrono::steady_clock::time_point last_audio_time_;
    bool idle_shutdown_triggered_ = false;

    // Sliding window logic
    std::string full_transcription_;     // filtered (hallucinations discarded)
    std::string raw_transcription_;      // unfiltered fallback — used when full_ is empty at handleEnd()

    // Signal flushLoop to stop and wait until it exits. Safe to call multiple times
    // (flush_thread_.joinable() returns false after the first join).
    void stopFlushLoop() {
        {
            std::lock_guard<std::mutex> lk(flush_cv_mutex_);
            flush_running_ = false;
        }
        flush_cv_.notify_one();
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
    }

    void flushLoop() {
        // Handles ALL inference, decoupled from the WebSocket receive loop.
        // Triggers on: 250ms of new audio accumulated, OR 400ms of silence with unprocessed audio.
        // Does NOT trigger if buffer < 2s: Whisper hallucinates badly on very short windows.
        const size_t MIN_NEW_SAMPLES    = msToSamples16kHz(flush_min_new_audio_ms_);
        const size_t MIN_BUFFER_SAMPLES = 32000; // 2s minimum before first inference

        while (flush_running_) {
            {
                std::unique_lock<std::mutex> lk(flush_cv_mutex_);
                flush_cv_.wait_for(lk, std::chrono::milliseconds(200),
                                   [this]{ return !flush_running_.load(); });
            }
            if (!flush_running_) break;

            std::unique_lock<std::mutex> lock(state_mutex_, std::defer_lock);
            if (!lock.try_lock()) {
                continue;
            }

            // Idle watchdog (jota-transcriber#68): a client that goes quiet without
            // ever sending a WS close frame — network partition, crash, or just
            // abandoning the connection — must not hang ws_.read() on the main
            // thread forever, since that leaks this session's ConnectionLimiter
            // slot indefinitely. SO_RCVTIMEO on the native socket doesn't reliably
            // interrupt that synchronous read, so force it from here instead:
            // this thread runs regardless of configured_/engine_ state, so an
            // unconfigured session (never sent "config") is covered too.
            if (session_timeout_sec_ > 0 && !idle_shutdown_triggered_) {
                auto idle_s = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - last_audio_time_).count();
                if (idle_s >= session_timeout_sec_) {
                    idle_shutdown_triggered_ = true;
                    Log::warn("Idle timeout (" + std::to_string(idle_s) + "s >= " +
                              std::to_string(session_timeout_sec_) +
                              "s), forcing socket shutdown", session_id_);
                    forceSocketShutdown(beast::get_lowest_layer(ws_).native_handle());
                }
            }

            if (!configured_ || !engine_) continue;

            size_t current_size = engine_->getBufferSize();

            // Guard: never infer on less than 2s of audio.
            if (current_size < MIN_BUFFER_SAMPLES) continue;

            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_audio_time_).count();

            bool enough_new_audio = (current_size - last_transcribed_size_) >= MIN_NEW_SAMPLES;
            bool silence_flush    = (elapsed_ms > 400) && (current_size > last_transcribed_size_);

            if (!enough_new_audio && !silence_flush) continue;

            Log::debug("flushLoop inference: new=" + std::to_string(current_size - last_transcribed_size_) +
                       " silence_ms=" + std::to_string(elapsed_ms), session_id_);
            // Non-blocking inference: skip cycle if GPU is saturated.
            // Lock order: state_mutex_ (already held) → InferenceLimiter::mutex_ (taken inside TryGuard).
            // No inversion risk: InferenceLimiter never acquires state_mutex_.
            InferenceLimiter::TryGuard inf_guard;
            if (!inf_guard.acquired()) {
                Log::debug("flushLoop: GPU busy, skipping inference cycle", session_id_);
                if (!capacity_degraded_) {
                    capacity_degraded_ = true;
                    Log::warn("flushLoop: entrando en estado busy (gpu_saturated)", session_id_);
                    lock.unlock();
                    sendMessage({{"type", "status"}, {"state", "busy"}, {"reason", "gpu_saturated"}});
                    lock.lock();
                }
                continue;
            }
            if (capacity_degraded_) {
                capacity_degraded_ = false;
                Log::info("flushLoop: saliendo de estado busy", session_id_);
                lock.unlock();
                sendMessage({{"type", "status"}, {"state", "ok"}});
                lock.lock();
            }
            auto res = engine_->transcribeSlidingWindow(false);
            // inf_guard releases the slot automatically on scope exit (including exceptions)

            // If inference drained the buffer below the low-water mark, the saturation
            // episode is over: report any drops not yet covered by a periodic warning,
            // tell the client it can resume, then reset for the next episode.
            constexpr size_t LOW_WATER_MARK = 16000 * 10;
            constexpr size_t WARNING_INTERVAL_CHUNKS = 10;
            if (buffer_overflowed_ && engine_->getBufferSize() < LOW_WATER_MARK) {
                // Snapshot while still locked — dropped_chunks_ is state_mutex_-protected
                // state written by processAudioChunk() on the WS receive thread, so it
                // must not be read unlocked from this (flushLoop) thread below.
                size_t dropped_snapshot = dropped_chunks_;
                if (dropped_snapshot % WARNING_INTERVAL_CHUNKS != 0) {
                    lock.unlock();
                    sendMessage({
                        {"type", "warning"},
                        {"code", "buffer_full"},
                        {"dropped_chunks", dropped_snapshot},
                        {"message", "Audio buffer full, dropping incoming audio"}
                    });
                    lock.lock();
                }
                Log::info("flushLoop: saliendo de pausa de flujo (dropped=" +
                          std::to_string(dropped_snapshot) + ")", session_id_);
                lock.unlock();
                sendMessage({{"type", "flow_control"}, {"action", "resume"}});
                lock.lock();
                buffer_overflowed_ = false;
                dropped_chunks_ = 0;
            }

            // Hallucination guard: filter loops before updating state or sending to client.
            bool committed_ok = !res.committed_text.empty() && !isHallucination(res.committed_text);
            bool partial_ok   = !res.partial_text.empty()   && !isHallucination(res.partial_text);

            if (!res.committed_text.empty()) {
                // Always accumulate raw — audio was already erased from engine buffer,
                // so this is the only copy left. Used as fallback in handleEnd() if all
                // commits were hallucination-filtered and full_transcription_ ends up empty.
                raw_transcription_ += res.committed_text;
                if (committed_ok) {
                    full_transcription_ += res.committed_text;
                } else {
                    Log::warn("Dropping hallucinated commit (len=" +
                              std::to_string(res.committed_text.length()) + "): '" +
                              res.committed_text.substr(0, 80) + "'", session_id_);
                }
                last_transcribed_size_ = engine_->getBufferSize();
            } else {
                last_transcribed_size_ = current_size;
            }

            if (!res.partial_text.empty() && !partial_ok) {
                Log::warn("Suppressing hallucinated partial (len=" +
                          std::to_string(res.partial_text.length()) + ")", session_id_);
            }

            if (committed_ok || partial_ok) {
                std::string merged = full_transcription_ + (partial_ok ? res.partial_text : "");
                json msg = {
                    {"type", "transcription"},
                    {"text", merged},
                    {"is_final", false}
                };
                lock.unlock();
                sendMessage(msg);
                lock.lock();
            }
        }
    }
};
