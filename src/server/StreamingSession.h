#pragma once
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <iostream>
#include "whisper/StreamingWhisperEngine.h"
#include "whisper/ModelCache.h"
#include "server/SessionTracker.h"
#include "AuthManager.h"
#include "ConnectionGuard.h"
#include "mqtt/MQTTPublisher.h"
#include "mqtt/TranscriptionEvent.h"
#include "log/Log.h"

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
        std::shared_ptr<MQTTPublisher> mqtt_publisher = nullptr
    )
        : ws_(std::move(ws)),
          model_path_(model_path),
          auth_manager_(auth_manager),
          mqtt_publisher_(mqtt_publisher),
          configured_(false),
          last_transcribed_size_(0),
          language_("es"),
          publish_mqtt_(false),
          whisper_beam_size_(whisper_beam_size),
          whisper_threads_(whisper_threads),
          whisper_initial_prompt_(whisper_initial_prompt),
          session_timeout_sec_(session_timeout_sec),
          model_acquired_(false),
          bytes_received_in_window_(0),
          rate_limit_start_(std::chrono::steady_clock::now())
    {
        session_id_ = generateSessionId();
        Log::info("Session created", session_id_);
        SessionTracker::instance().add(this);
    }

    ~StreamingSession() override {
        SessionTracker::instance().remove(this);
        releaseModel();
    }

    void shutdown() override {
        // Triggered asynchronously by signal handler. We try to close cleanly if we can.
        boost::system::error_code ec;
        beast::get_lowest_layer(ws_).cancel(ec);
    }

    template<class Req>
    void run(const Req& req) {
        try {
            if (session_timeout_sec_ > 0) {
                // Set native socket receive timeout to drop idle/zombie connections
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
        if (model_acquired_) {
            engine_.reset();
            ModelCache::instance().release();
            model_acquired_ = false;
            Log::info("Model reference released", session_id_);
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
        // Enforce Binary Rate Limit (QoS)
        auto now = std::chrono::steady_clock::now();
        auto elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(now - rate_limit_start_).count();
        
        bytes_received_in_window_ += data.size();
        
        if (elapsed_s >= 3) {
            // max 200 KB/s over 3 seconds = 600 KB window limit
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

        if (data.size() < 44) { // WAV header is 44 bytes, so anything smaller is not a valid audio file
            Log::warn("Received binary data too small to be a WAV file (" + std::to_string(data.size()) + " bytes)", session_id_);
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

            if (msg.contains("publish_mqtt") && msg["publish_mqtt"].is_boolean()) {
                publish_mqtt_ = msg["publish_mqtt"].get<bool>();
                if (publish_mqtt_ && !mqtt_publisher_) {
                    Log::warn("Client requested MQTT publish but MQTT is not configured", session_id_);
                    publish_mqtt_ = false;
                }
            }

            // VAD configure (0.0 = disabled, try a safe 0.4 for long silences only if enabled by client)
            float vad_thold = 0.0f;
            if (msg.contains("vad_thold") && msg["vad_thold"].is_number()) {
                vad_thold = msg["vad_thold"].get<float>();
            }

            // Acquire model from cache (loads if not already loaded, instant if cached)
            Log::info("Acquiring model from cache: " + model_path_, session_id_);
            whisper_context* ctx = ModelCache::instance().acquire(model_path_);
            model_acquired_ = true;

            // Create engine with shared context (creates its own whisper_state)
            engine_ = std::make_unique<StreamingWhisperEngine>(ctx);
            engine_->setLanguage(language_);
            engine_->setThreads(whisper_threads_);
            engine_->setBeamSize(whisper_beam_size_);
            engine_->setVadThreshold(vad_thold);
            if (!whisper_initial_prompt_.empty()) {
                engine_->setInitialPrompt(whisper_initial_prompt_);
            }

            configured_ = true;
            last_transcribed_size_ = 0;
            transcription_offset_  = 0;
            full_transcription_    = "";

            Log::info("Session ready (lang=" + language_ +
                      ", beam=" + std::to_string(whisper_beam_size_) +
                      ", vad=" + std::to_string(vad_thold) +
                      ", mqtt=" + (publish_mqtt_ ? "on" : "off") + ")", session_id_);
            sendReady();
        }
        catch (std::exception& e) {
            Log::error(std::string("Config failed: ") + e.what(), session_id_);
            sendError("Configuration failed: " + std::string(e.what()), "CONFIG_ERROR");
        }
    }

    void handleEnd() {
        Log::info("End-of-stream received, running final transcription", session_id_);

        if (engine_) {
            // Transcribe remaining buffer
            std::string finalText = engine_->transcribe(transcription_offset_);
            if (!finalText.empty()) {
                // If it's just the continuation, append it
                full_transcription_ += finalText;
            }

            Log::info("Final transcription: \"" + full_transcription_ + "\"", session_id_);
            json msg = {
                {"type", "transcription"},
                {"text", full_transcription_},
                {"is_final", true}
            };
            sendMessage(msg);

            if (publish_mqtt_) {
                auto event = TranscriptionEvent::make(session_id_, full_transcription_, true, language_);
                if (!mqtt_publisher_->publishTranscription(event)) {
                    Log::warn("MQTT publish failed for session", session_id_);
                }
            }
        }

        try {
            ws_.close(websocket::close_code::normal);
            Log::info("Session closed normally", session_id_);
        }
        catch (std::exception& e) {
            Log::error(std::string("Error closing WebSocket: ") + e.what(), session_id_);
        }
    }

    void processAudioChunk(const std::vector<float>& audio) {
        if (!configured_ || !engine_) return;

        engine_->processAudioChunk(audio);

        size_t current_size = engine_->getBufferSize();
        
        // Triggers roughly every 1 second of new audio (16kHz)
        const size_t MIN_NEW_SAMPLES = 16000; 
        // Sliding window size: e.g. 5 seconds (16000 * 5)
        const size_t WINDOW_SIZE = 16000 * 5;

        if (current_size >= MIN_NEW_SAMPLES &&
            (current_size - last_transcribed_size_ >= MIN_NEW_SAMPLES)) {

            last_transcribed_size_ = current_size;
            
            // To prevent O(n^2), we only transcribe from an offset, looking roughly at the last WINDOW_SIZE
            // We use overlapping to avoid clipping words
            if (current_size > WINDOW_SIZE + transcription_offset_) {
                // We've accumulated enough beyond the window.
                // Transcribe the finalized window exactly.
                std::string finalized_text = engine_->transcribe(transcription_offset_);
                full_transcription_ += finalized_text;
                
                // Shift the offset forward, leaving ~1.5s overlapping context (16000 * 1.5) to avoid cutting words
                size_t keep_samples = static_cast<size_t>(16000 * 1.5);
                engine_->reset(keep_samples);
                
                // Now buffer is tiny again.
                current_size = engine_->getBufferSize();
                last_transcribed_size_ = current_size;
                transcription_offset_ = 0;
            }

            // Transcribe the working window
            std::string partialText = engine_->transcribe(transcription_offset_);

            if (!partialText.empty()) {
                std::string merged_so_far = full_transcription_ + partialText;
                // Log::debug("Partial transcription: \"" + merged_so_far + "\"", session_id_);
                json msg = {
                    {"type", "transcription"},
                    {"text", merged_so_far},
                    {"is_final", false}
                };
                sendMessage(msg);
            }
        }
    }

    void sendReady() {
        json msg = {
            {"type", "ready"},
            {"session_id", session_id_},
            {"config", {
                {"language", language_},
                {"sample_rate", 16000},
                {"beam_size", whisper_beam_size_},
                {"publish_mqtt", publish_mqtt_}
            }}
        };
        sendMessage(msg);
    }

    void sendError(const std::string& message, const std::string& code) {
        json msg = {
            {"type", "error"},
            {"message", message},
            {"code", code}
        };
        sendMessage(msg);
    }

    void sendMessage(const json& msg) {
        try {
            std::string str = msg.dump();
            ws_.text(true);
            ws_.write(net::buffer(str));
        }
        catch (std::exception& e) {
            Log::error(std::string("Failed to send message: ") + e.what(), session_id_);
        }
    }

    std::string generateSessionId() {
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(1000, 9999);

        std::ostringstream oss;
        oss << "session-" << timestamp << "-" << dis(gen);
        return oss.str();
    }

    // Member variables
    StreamType ws_;
    std::string model_path_;
    std::shared_ptr<AuthManager> auth_manager_;
    std::shared_ptr<MQTTPublisher> mqtt_publisher_;
    bool publish_mqtt_;
    std::unique_ptr<StreamingWhisperEngine> engine_;
    std::string session_id_;
    bool configured_;
    size_t last_transcribed_size_;
    std::string language_;

    // Whisper params
    int whisper_beam_size_;
    int whisper_threads_;
    std::string whisper_initial_prompt_;
    int session_timeout_sec_;
    bool model_acquired_;
    
    // QoS Rate limiting
    size_t bytes_received_in_window_;
    std::chrono::steady_clock::time_point rate_limit_start_;

    // Sliding window logic
    size_t transcription_offset_;
    std::string full_transcription_;
};
