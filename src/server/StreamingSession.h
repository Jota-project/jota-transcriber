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
class StreamingSession : public std::enable_shared_from_this<StreamingSession<StreamType>> {
public:
    StreamingSession(
        StreamType&& ws,
        const std::string& model_path,
        std::shared_ptr<AuthManager> auth_manager,
        int whisper_beam_size = 5,
        int whisper_threads = 4,
        const std::string& whisper_initial_prompt = "",
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
          model_acquired_(false)
    {
        session_id_ = generateSessionId();
        Log::info("Session created", session_id_);
    }

    ~StreamingSession() {
        releaseModel();
    }

    void run() {
        try {
            ws_.accept();
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

            // Acquire model from cache (loads if not already loaded, instant if cached)
            Log::info("Acquiring model from cache: " + model_path_, session_id_);
            whisper_context* ctx = ModelCache::instance().acquire(model_path_);
            model_acquired_ = true;

            // Create engine with shared context (creates its own whisper_state)
            engine_ = std::make_unique<StreamingWhisperEngine>(ctx);
            engine_->setLanguage(language_);
            engine_->setThreads(whisper_threads_);
            engine_->setBeamSize(whisper_beam_size_);
            if (!whisper_initial_prompt_.empty()) {
                engine_->setInitialPrompt(whisper_initial_prompt_);
            }

            configured_ = true;
            last_transcribed_size_ = 0;

            Log::info("Session ready (lang=" + language_ +
                      ", beam=" + std::to_string(whisper_beam_size_) +
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
            std::string finalText = engine_->transcribe();
            Log::info("Final transcription: \"" + finalText + "\"", session_id_);
            json msg = {
                {"type", "transcription"},
                {"text", finalText},
                {"is_final", true}
            };
            sendMessage(msg);

            if (publish_mqtt_) {
                auto event = TranscriptionEvent::make(session_id_, finalText, true, language_);
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
        if (current_size < last_transcribed_size_) {
            last_transcribed_size_ = 0;
        }

        const size_t MIN_NEW_SAMPLES = 16000;
        if (current_size >= MIN_NEW_SAMPLES &&
            (current_size - last_transcribed_size_ >= MIN_NEW_SAMPLES)) {

            last_transcribed_size_ = current_size;
            std::string partialText = engine_->transcribe();

            if (!partialText.empty()) {
                Log::debug("Partial transcription: \"" + partialText + "\"", session_id_);
                json msg = {
                    {"type", "transcription"},
                    {"text", partialText},
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
    bool model_acquired_;
};
