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
#include "whisper/StreamingWhisperEngine.h"
#include "whisper/ModelCache.h"
#include "server/SessionTracker.h"
#include "AuthManager.h"
#include "ConnectionGuard.h"
#include "log/Log.h"
#include "utils/HallucinationGuard.h"
#include "whisper/InferenceLimiter.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::json;

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
        float whisper_logprob_thold = -1.0f
    )
        : ws_(std::move(ws)),
          model_path_(model_path),
          auth_manager_(auth_manager),
          configured_(false),
          buffer_overflowed_(false),
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
          model_acquired_(false),
          bytes_received_in_window_(0),
          rate_limit_start_(std::chrono::steady_clock::now()),
          flush_running_(false),
          last_audio_time_(std::chrono::steady_clock::now()),
          hallucination_config_(getHallucinationConfigForModel(model_path))
    {
        session_id_ = generateSessionId();
        Log::info("Session created (hallucination_config: max_consec=" +
 std::to_string(hallucination_config_.max_consecutive) +
                  ", max_bigrams=" + std::to_string(hallucination_config_.max_repeated_bigrams) + ")",
                  session_id_);
        SessionTracker::instance().add(this);
    }

    ~StreamingSession() override {
        stopFlushLoop();
        SessionTracker::instance().remove(this);
        releaseModel();
    }

    void shutdown() override {
        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).cancel(ec);
    }

    template<class Req>
    void run(const Req& req) {
        try {
            if (session_timeout_sec_ > 0) {
                auto native_socket = beast::get_lowest_layer(ws_).native_handle();
#if defined(_WIN32)
                DWORD timeout = session_timeout_sec_ * 1000;
                setsockopt(native_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
                struct timeval tv;
                tv.tv_sec = session_timeout_sec_;
                tv.tv_usec = 0;
                setsockopt(native_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            }

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
        bool should_warn = overflow && !buffer_overflowed_;
        buffer_overflowed_ = overflow;
        lock.unlock();

        if (should_warn) {
            Log::warn("Audio buffer full, dropping incoming audio", session_id_);
            json warning = {
                {"type", "warning"},
                {"code", "buffer_full"},
                {"message", "Audio buffer full, dropping incoming audio"}
            };
            sendMessage(warning);
        }
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
        constexpr size_t MAX_FRAME_BYTES = 1 * 1024 * 1024;
        if (data.size() > MAX_FRAME_BYTES) {
            Log::warn("Binary frame too large (" + std::to_string(data.size()) +
                      " bytes > 1MB limit), closing connection", session_id_);
            boost::system::error_code ec;
            ws_.close(websocket::close_reason(websocket::close_code::policy_error, "Frame too large"), ec);
            return;
        }

        if (data.size() < sizeof(float)) {
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

        auto now = std::chrono::steady_clock::now();
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - rate_limit_start_).count();

        bytes_received_in_window_ += data.size();

        if (elapsed_s >= 3) {
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

            float vad_thold = 0.0f;
            if (msg.contains("vad_thold") && msg["vad_thold"].is_number()) {
                vad_thold = msg["vad_thold"].get<float>();
            }

            Log::info("Acquiring model from cache: " + model_path_, session_id_);
            whisper_context* ctx = ModelCache::instance().acquire(model_path_);

            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                model_acquired_ = true;

                engine_ = std::make_unique<StreamingWhisperEngine>(ctx);
                engine_->setLanguage(language_);
                engine_->setThreads(whisper_threads_);
                engine_->setBeamSize(whisper_beam_size_);
                engine_->setVadThreshold(vad_thold);
                engine_->setTemperature(whisper_temperature_);
                engine_->setTemperatureInc(whisper_temperature_inc_);
                engine_->setNoSpeechThreshold(whisper_no_speech_thold_);
                engine_->setLogprobThreshold(whisper_logprob_thold_);
                if (!whisper_initial_prompt_.empty()) {
                    engine_->setInitialPrompt(whisper_initial_prompt_);
                }

                configured_ = true;
                last_transcribed_size_ = 0;
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

        stopFlushLoop();

        std::string final_text;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (engine_) {
                auto res = engine_->transcribeSlidingWindow(true);
                std::string final_text_to_add = res.committed_text;
                if (isHallucination(res.committed_text, hallucination_config_)) {
                    final_text_to_add = cleanHallucination(res.committed_text, hallucination_config_);
                    Log::warn("Final commit hallucination cleaned (orig_len=" +
                              std::to_string(res.committed_text.length()) +
                              ", clean_len=" + std::to_string(final_text_to_add.length()) + ")", session_id_);
                }
                if (!final_text_to_add.empty()) {
                    full_transcription_ += final_text_to_add;
                }
            }

            if (full_transcription_.empty() && !raw_transcription_.empty()) {
                Log::warn("full_transcription_ empty at end-of-session — using raw fallback", session_id_);
                full_transcription_ = raw_transcription_;
            }

            final_text = full_transcription_;
        }

        Log::info("Final transcription: \"" + final_text + "\"", session_id_);
        json msg = {
            {"type", "transcription"},
            {"text", final_text},
            {"is_final", true}
        };
        sendMessage(msg);

        try {
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

    StreamType ws_;
    std::string model_path_;
    std::shared_ptr<AuthManager> auth_manager_;
    std::unique_ptr<StreamingWhisperEngine> engine_;
    std::string session_id_;
    bool configured_;
    bool buffer_overflowed_;
    size_t last_transcribed_size_;
    std::string language_;

    int whisper_beam_size_;
    int whisper_threads_;
    std::string whisper_initial_prompt_;
    int session_timeout_sec_;
    float whisper_temperature_;
    float whisper_temperature_inc_;
    float whisper_no_speech_thold_;
    float whisper_logprob_thold_;
    bool model_acquired_;

    size_t bytes_received_in_window_;
    std::chrono::steady_clock::time_point rate_limit_start_;

    std::mutex write_mutex_;
    std::mutex state_mutex_;
    std::thread flush_thread_;
    std::atomic<bool> flush_running_;
    std::mutex flush_cv_mutex_;
    std::condition_variable flush_cv_;
    std::chrono::steady_clock::time_point last_audio_time_;

    std::string full_transcription_;
    std::string raw_transcription_;

    HallucinationConfig hallucination_config_;

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
        const size_t MIN_NEW_SAMPLES    = 4000;
        const size_t MIN_BUFFER_SAMPLES = 32000;

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

            if (!configured_ || !engine_) continue;

            size_t current_size = engine_->getBufferSize();

            if (current_size < MIN_BUFFER_SAMPLES) continue;

            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_audio_time_).count();

            bool enough_new_audio = (current_size - last_transcribed_size_) >= MIN_NEW_SAMPLES;
            bool silence_flush    = (elapsed_ms > 400) && (current_size > last_transcribed_size_);

            if (!enough_new_audio && !silence_flush) continue;

            Log::debug("flushLoop inference: new=" + std::to_string(current_size - last_transcribed_size_) +
                       " silence_ms=" + std::to_string(elapsed_ms), session_id_);
            InferenceLimiter::TryGuard inf_guard;
            if (!inf_guard.acquired()) {
                Log::debug("flushLoop: GPU busy, skipping inference cycle", session_id_);
                continue;
            }
            auto res = engine_->transcribeSlidingWindow(false);

            constexpr size_t HIGH_WATER_MARK = 16000 * 20;
            if (buffer_overflowed_ && engine_->getBufferSize() < HIGH_WATER_MARK) {
                buffer_overflowed_ = false;
            }

            bool committed_hallucinated = isHallucination(res.committed_text, hallucination_config_);
            bool partial_hallucinated = isHallucination(res.partial_text, hallucination_config_);

            std::string committed_text_to_use = res.committed_text;
            std::string partial_text_to_use = res.partial_text;

            if (committed_hallucinated && !res.committed_text.empty()) {
                committed_text_to_use = cleanHallucination(res.committed_text, hallucination_config_);
                Log::warn("Hallucination detected in commit, cleaned (orig_len=" +
                          std::to_string(res.committed_text.length()) +
                          ", clean_len=" + std::to_string(committed_text_to_use.length()) + "): '" +
                          committed_text_to_use.substr(0, 80) + "'", session_id_);
            }

            if (partial_hallucinated && !res.partial_text.empty()) {
                partial_text_to_use = cleanHallucination(res.partial_text, hallucination_config_);
                Log::warn("Hallucination detected in partial, cleaned (orig_len=" +
                          std::to_string(res.partial_text.length()) +
                          ", clean_len=" + std::to_string(partial_text_to_use.length()) + "): '" +
                          partial_text_to_use.substr(0, 80) + "'", session_id_);
            }

            if (!res.committed_text.empty()) {
                raw_transcription_ += res.committed_text;
                if (!committed_text_to_use.empty()) {
                    full_transcription_ += committed_text_to_use;
                }
                last_transcribed_size_ = engine_->getBufferSize();
            } else {
                last_transcribed_size_ = current_size;
            }

            if (!committed_text_to_use.empty() || !partial_text_to_use.empty()) {
                std::string merged = full_transcription_ + partial_text_to_use;
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
