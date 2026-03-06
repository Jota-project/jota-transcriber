#include "MQTTPublisher.h"
#include "MQTTConfig.h"
#include <nlohmann/json.hpp>
#include "log/Log.h"

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// MQTTConfig helpers
// ---------------------------------------------------------------------------

MQTTConfig MQTTConfig::fromUrl(const std::string& url,
                                const std::string& topic,
                                const std::string& client_id)
{
    MQTTConfig cfg;
    cfg.topic     = topic;
    cfg.client_id = client_id;

    // Expected format: mqtt://host:port
    const std::string scheme = "mqtt://";
    if (url.size() <= scheme.size() || url.substr(0, scheme.size()) != scheme)
        return cfg; // return defaults

    std::string remainder = url.substr(scheme.size()); // "host:port"
    auto colon = remainder.rfind(':');
    if (colon == std::string::npos) {
        cfg.broker_host = remainder;
    } else {
        cfg.broker_host = remainder.substr(0, colon);
        try {
            cfg.broker_port = std::stoi(remainder.substr(colon + 1));
        } catch (...) {
            // keep default port
        }
    }

    return cfg;
}

MQTTConfig MQTTConfig::fromEnv(const std::string& env_var,
                                const std::string& topic,
                                const std::string& client_id)
{
    const char* val = std::getenv(env_var.c_str());
    if (!val || std::string(val).empty())
        return MQTTConfig{};
    return MQTTConfig::fromUrl(val, topic, client_id);
}

// ---------------------------------------------------------------------------
// MQTTPublisher
// ---------------------------------------------------------------------------

MQTTPublisher::MQTTPublisher(const MQTTConfig& config)
    : mosqpp::mosquittopp(config.client_id.c_str()),
      config_(config)
{
    mosqpp::lib_init();
}

MQTTPublisher::~MQTTPublisher()
{
    if (connected_) {
        disconnect();
    }
    mosqpp::lib_cleanup();
}

bool MQTTPublisher::connect()
{
    int rc = mosqpp::mosquittopp::connect(
        config_.broker_host.c_str(),
        config_.broker_port,
        config_.keepalive_secs
    );

    if (rc != MOSQ_ERR_SUCCESS) {
        Log::error(std::string("[MQTT] connect() failed: ") + mosquitto_strerror(rc) +
                   " (" + config_.broker_host + ":" + std::to_string(config_.broker_port) + ")");
        return false;
    }

    loop_start(); // background thread for I/O
    return true;
}

void MQTTPublisher::disconnect()
{
    mosqpp::mosquittopp::disconnect();
    loop_stop();
    connected_ = false;
}

bool MQTTPublisher::isConnected() const
{
    return connected_;
}

bool MQTTPublisher::publishTranscription(const TranscriptionEvent& event)
{
    if (!event.is_final)
        return true; // only final transcriptions are published

    if (!connected_) {
        Log::warn("[MQTT] not connected, dropping final transcription", event.session_id);
        return false;
    }

    const std::string topic   = config_.topic;
    const std::string payload = buildPayload(event);

    int rc = publish(
        nullptr,               // mid (not needed)
        topic.c_str(),
        static_cast<int>(payload.size()),
        payload.c_str(),
        config_.qos,
        config_.retain
    );

    if (rc != MOSQ_ERR_SUCCESS) {
        Log::error(std::string("[MQTT] publish() failed: ") + mosquitto_strerror(rc) +
                   " topic=" + topic);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string MQTTPublisher::buildTopic() const
{
    return config_.topic;
}

std::string MQTTPublisher::buildPayload(const TranscriptionEvent& event) const
{
    json payload = {
        {"session_id",    event.session_id},
        {"text",          event.text},
        {"is_final",      event.is_final},
        {"language",      event.language},
        {"timestamp_ms",  event.timestamp_ms}
    };
    return payload.dump();
}

// ---------------------------------------------------------------------------
// mosquittopp callbacks
// ---------------------------------------------------------------------------

void MQTTPublisher::on_connect(int rc)
{
    if (rc == MOSQ_ERR_SUCCESS) {
        connected_ = true;
        Log::info("[MQTT] Connected to " + config_.broker_host + ":" + std::to_string(config_.broker_port));
    } else {
        Log::error(std::string("[MQTT] on_connect error: ") + mosquitto_strerror(rc));
    }
}

void MQTTPublisher::on_disconnect(int rc)
{
    connected_ = false;
    if (rc != MOSQ_ERR_SUCCESS) {
        Log::warn(std::string("[MQTT] Unexpected disconnect: ") + mosquitto_strerror(rc));
    }
}

void MQTTPublisher::on_publish(int /*mid*/)
{
    // Available for debug tracing if needed.
}
