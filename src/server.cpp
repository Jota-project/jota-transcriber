#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <cstdlib>
#include "server/StreamingSession.h"
#include "server/ServerConfig.h"
#include "server/ConnectionLimiter.h"
#include "server/ConnectionGuard.h"
#include "server/AuthManager.h"
#include "auth/ApiAuthConfig.h"
#include "mqtt/MQTTPublisher.h"
#include "whisper/ModelCache.h"
#include "whisper/InferenceLimiter.h"
#include "server/SessionTracker.h"
#include "log/Log.h"

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
namespace ssl = boost::asio::ssl;

namespace {

// ---------------------------------------------------------------------------
// .env loader
// ---------------------------------------------------------------------------
void loadDotEnv(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        if (value.size() >= 2) {
            char q = value.front();
            if ((q == '"' || q == '\'') && value.back() == q) {
                value = value.substr(1, value.size() - 2);
            }
        }

        if (!key.empty() && ::getenv(key.c_str()) == nullptr) {
            ::setenv(key.c_str(), value.c_str(), 0);
        }
    }
}

std::string env(const char* var) {
    const char* v = ::getenv(var);
    return v ? std::string(v) : std::string{};
}

ServerConfig configFromEnv() {
    ServerConfig cfg;

    if (auto v = env("MODEL_PATH"); !v.empty())
        cfg.model_path = v;

    if (auto v = env("BIND_ADDRESS"); !v.empty())
        cfg.bind_address = v;

    if (auto v = env("PORT"); !v.empty())
        cfg.port = static_cast<unsigned short>(std::stoul(v));

    if (auto v = env("AUTH_API_URL"); !v.empty())
        cfg.auth_api_url = v;

    if (auto v = env("AUTH_API_SECRET"); !v.empty())
        cfg.auth_api_secret = v;

    if (auto v = env("AUTH_CACHE_TTL"); !v.empty())
        cfg.auth_cache_ttl = std::stoi(v);

    if (auto v = env("AUTH_API_TIMEOUT"); !v.empty())
        cfg.auth_api_timeout = std::stoi(v);

    if (auto v = env("TLS_CERT"); !v.empty())
        cfg.cert_path = v;

    if (auto v = env("TLS_KEY"); !v.empty())
        cfg.key_path = v;

    if (auto v = env("MAX_CONNECTIONS"); !v.empty())
        cfg.max_connections = static_cast<size_t>(std::stoul(v));

    if (auto v = env("MAX_CONNECTIONS_PER_IP"); !v.empty())
        cfg.max_connections_per_ip = static_cast<size_t>(std::stoul(v));

    if (auto v = env("MQTT_URL"); !v.empty())
        cfg.mqtt_url = v;

    if (auto v = env("MQTT_TOPIC"); !v.empty())
        cfg.mqtt_topic = v;

    if (auto v = env("MQTT_CLIENT_ID"); !v.empty())
        cfg.mqtt_client_id = v;

    // Whisper quality params
    if (auto v = env("WHISPER_BEAM_SIZE"); !v.empty())
        cfg.whisper_beam_size = std::stoi(v);

    if (auto v = env("WHISPER_THREADS"); !v.empty())
        cfg.whisper_threads = std::stoi(v);

    if (auto v = env("MAX_CONCURRENT_INFERENCE"); !v.empty())
        cfg.max_concurrent_inference = std::stoi(v);

    if (auto v = env("MODEL_CACHE_TTL"); !v.empty())
        cfg.model_cache_ttl = std::stoi(v);

    if (auto v = env("WHISPER_INITIAL_PROMPT"); !v.empty())
        cfg.whisper_initial_prompt = v;

    if (auto v = env("SESSION_TIMEOUT_SEC"); !v.empty())
        cfg.session_timeout_sec = std::stoi(v);

    return cfg;
}

// ---------------------------------------------------------------------------

void printUsage(const char* binary) {
    std::cout << "Usage: " << binary
              << " [--model path] [--bind address] [--port N]"
              << " [--auth-api-url URL] [--auth-api-secret SECRET]"
              << " [--auth-cache-ttl N] [--auth-api-timeout N]"
              << " [--cert cert.pem] [--key key.pem]"
              << " [--max-connections N] [--max-connections-per-ip N]"
              << " [--mqtt-url URL] [--mqtt-topic TOPIC]"
              << " [--whisper-beam-size N] [--whisper-threads N]"
              << " [--max-concurrent-inference N] [--model-cache-ttl N]"
              << " [--whisper-initial-prompt TEXT] [--session-timeout-sec N]"
              << " [--env-file path]" << std::endl;
    std::cout << "All options can also be set via environment variables (or a .env file):" << std::endl;
    std::cout << "  MODEL_PATH, BIND_ADDRESS, PORT," << std::endl;
    std::cout << "  AUTH_API_URL, AUTH_API_SECRET, AUTH_CACHE_TTL, AUTH_API_TIMEOUT," << std::endl;
    std::cout << "  TLS_CERT, TLS_KEY, MAX_CONNECTIONS, MAX_CONNECTIONS_PER_IP," << std::endl;
    std::cout << "  MQTT_URL, MQTT_TOPIC, MQTT_CLIENT_ID," << std::endl;
    std::cout << "  WHISPER_BEAM_SIZE, WHISPER_THREADS, MAX_CONCURRENT_INFERENCE," << std::endl;
    std::cout << "  MODEL_CACHE_TTL, WHISPER_INITIAL_PROMPT, SESSION_TIMEOUT_SEC" << std::endl;
    std::cout << "CLI arguments override environment variables." << std::endl;
}

std::string extractEnvFile(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--env-file" && i + 1 < argc) {
            return argv[i + 1];
        }
    }
    return ".env";
}

ServerConfig parseArgs(int argc, char* argv[]) {
    ServerConfig config = configFromEnv();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--env-file" && i + 1 < argc) {
            ++i;
        } else if (arg == "--model" && i + 1 < argc) {
            config.model_path = argv[++i];
        } else if (arg == "--bind" && i + 1 < argc) {
            config.bind_address = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<unsigned short>(std::stoul(argv[++i]));
        } else if (arg == "--auth-api-url" && i + 1 < argc) {
            config.auth_api_url = argv[++i];
        } else if (arg == "--auth-api-secret" && i + 1 < argc) {
            config.auth_api_secret = argv[++i];
        } else if (arg == "--auth-cache-ttl" && i + 1 < argc) {
            config.auth_cache_ttl = std::stoi(argv[++i]);
        } else if (arg == "--auth-api-timeout" && i + 1 < argc) {
            config.auth_api_timeout = std::stoi(argv[++i]);
        } else if (arg == "--cert" && i + 1 < argc) {
            config.cert_path = argv[++i];
        } else if (arg == "--key" && i + 1 < argc) {
            config.key_path = argv[++i];
        } else if (arg == "--max-connections" && i + 1 < argc) {
            config.max_connections = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--max-connections-per-ip" && i + 1 < argc) {
            config.max_connections_per_ip = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (arg == "--mqtt-url" && i + 1 < argc) {
            config.mqtt_url = argv[++i];
        } else if (arg == "--mqtt-topic" && i + 1 < argc) {
            config.mqtt_topic = argv[++i];
        } else if (arg == "--whisper-beam-size" && i + 1 < argc) {
            config.whisper_beam_size = std::stoi(argv[++i]);
        } else if (arg == "--whisper-threads" && i + 1 < argc) {
            config.whisper_threads = std::stoi(argv[++i]);
        } else if (arg == "--max-concurrent-inference" && i + 1 < argc) {
            config.max_concurrent_inference = std::stoi(argv[++i]);
        } else if (arg == "--model-cache-ttl" && i + 1 < argc) {
            config.model_cache_ttl = std::stoi(argv[++i]);
        } else if (arg == "--whisper-initial-prompt" && i + 1 < argc) {
            config.whisper_initial_prompt = argv[++i];
        } else if (arg == "--session-timeout-sec" && i + 1 < argc) {
            config.session_timeout_sec = std::stoi(argv[++i]);
        } else if (arg == "--thread-safe") {
            // accepted for backwards compatibility
        } else if (arg.rfind("--", 0) != 0 &&
                   config.model_path == "third_party/whisper.cpp/models/ggml-base.bin") {
            config.model_path = arg;
        } else {
            Log::error("Unknown argument: " + arg);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    return config;
}

void handleSession(tcp::socket socket,
                   const std::shared_ptr<ConnectionLimiter>& limiter,
                   const std::string& client_ip,
                   const std::string& model_path,
                   const std::shared_ptr<AuthManager>& auth_manager,
                   int whisper_beam_size,
                   int whisper_threads,
                   const std::string& whisper_initial_prompt,
                   int session_timeout_sec,
                   std::shared_ptr<ssl::context> ssl_ctx,
                   std::shared_ptr<MQTTPublisher> mqtt_publisher) {
    ConnectionGuard guard(limiter, client_ip);

    try {
        // Check for /metrics endpoint before upgrading to WebSocket
        boost::beast::flat_buffer buffer;
        boost::beast::http::request<boost::beast::http::string_body> req;
        boost::beast::http::read(socket, buffer, req);

        if (req.target() == "/metrics") {
            std::string inf_metrics = InferenceLimiter::instance().getMetrics();
            std::string cache_metrics = ModelCache::instance().getMetrics();
            std::string conn_metrics = limiter->getMetrics();

            std::string combined = 
                "# HELP transcription_active_inferences Number of concurrent inferences\n"
                "# TYPE transcription_active_inferences gauge\n" +
                inf_metrics +
                "# HELP transcription_model_loaded Whether the model is currently in memory (1=yes, 0=no)\n"
                "# TYPE transcription_model_loaded gauge\n" +
                cache_metrics +
                "# HELP transcription_active_connections Number of active WebSocket connections\n"
                "# TYPE transcription_active_connections gauge\n" +
                conn_metrics;

            boost::beast::http::response<boost::beast::http::string_body> res;
            res.version(req.version());
            res.result(boost::beast::http::status::ok);
            res.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(boost::beast::http::field::content_type, "text/plain; version=0.0.4");
            res.body() = combined;
            res.prepare_payload();
            boost::beast::http::write(socket, res);
            return; // Metrics served, close connection
        } else if (req.target() == "/health") {
            boost::beast::http::response<boost::beast::http::string_body> res;
            res.version(req.version());
            res.result(boost::beast::http::status::ok);
            res.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(boost::beast::http::field::content_type, "application/json");
            res.body() = "{\"status\": \"ok\"}";
            res.prepare_payload();
            boost::beast::http::write(socket, res);
            return; // close connection
        } else if (req.target() == "/ready") {
            // Check if server is too busy
            bool is_busy = !InferenceLimiter::instance().hasCapacity();
            boost::beast::http::response<boost::beast::http::string_body> res;
            res.version(req.version());
            res.result(is_busy ? boost::beast::http::status::service_unavailable : boost::beast::http::status::ok);
            res.set(boost::beast::http::field::server, BOOST_BEAST_VERSION_STRING);
            res.set(boost::beast::http::field::content_type, "application/json");
            res.body() = is_busy ? "{\"status\": \"busy\"}" : "{\"status\": \"ready\"}";
            res.prepare_payload();
            boost::beast::http::write(socket, res);
            return; // close connection
        }

        if (ssl_ctx) {
            websocket::stream<ssl::stream<tcp::socket>> ws(std::move(socket), *ssl_ctx);
            ws.next_layer().handshake(ssl::stream_base::server);
            auto session = std::make_shared<StreamingSession<websocket::stream<ssl::stream<tcp::socket>>>>(
                std::move(ws), model_path, auth_manager,
                whisper_beam_size, whisper_threads, whisper_initial_prompt,
                session_timeout_sec, mqtt_publisher
            );
            session->run();
        } else {
            websocket::stream<tcp::socket> ws(std::move(socket));
            auto session = std::make_shared<StreamingSession<websocket::stream<tcp::socket>>>(
                std::move(ws), model_path, auth_manager,
                whisper_beam_size, whisper_threads, whisper_initial_prompt,
                session_timeout_sec, mqtt_publisher
            );
            session->run();
        }
    } catch (std::exception& e) {
        Log::error("Session exception from " + client_ip + ": " + e.what());
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        loadDotEnv(extractEnvFile(argc, argv));

        ServerConfig config = parseArgs(argc, argv);

        std::cout << "Streaming Transcription Server\n";
        std::cout << "──────────────────────────────\n";

        bool use_ssl    = !config.cert_path.empty() && !config.key_path.empty();
        bool auth_enabled = !config.auth_api_url.empty();

        Log::info("Model:   " + config.model_path);
        Log::info("Bind:    " + config.bind_address + ":" + std::to_string(config.port));
        Log::info("SSL:     " + std::string(use_ssl ? "enabled" : "disabled"));
        if (auth_enabled) {
            Log::info("Auth:    API " + config.auth_api_url +
                      "  cache=" + std::to_string(config.auth_cache_ttl) + "s" +
                      "  timeout=" + std::to_string(config.auth_api_timeout) + "s");
        } else {
            Log::info("Auth:    disabled");
        }
        Log::info("Limits:  " + std::to_string(config.max_connections) + " total, " +
                  std::to_string(config.max_connections_per_ip) + " per IP");
        Log::info("Whisper: beam_size=" + std::to_string(config.whisper_beam_size) +
                  "  threads=" + std::to_string(config.whisper_threads) +
                  "  max_concurrent=" + std::to_string(config.max_concurrent_inference) +
                  "  cache_ttl=" + std::to_string(config.model_cache_ttl) + "s");
        if (!config.whisper_initial_prompt.empty()) {
            Log::info("Whisper: initial_prompt=\"" + config.whisper_initial_prompt + "\"");
        }

        // Configure the model cache and inference limiter
        ModelCache::instance().configure(config.model_cache_ttl);
        InferenceLimiter::instance().setMaxConcurrency(config.max_concurrent_inference);

        std::shared_ptr<ssl::context> ssl_ctx;
        if (use_ssl) {
            try {
                ssl_ctx = std::make_shared<ssl::context>(ssl::context::tlsv12);
                ssl_ctx->use_certificate_chain_file(config.cert_path);
                ssl_ctx->use_private_key_file(config.key_path, ssl::context::pem);
                Log::info("SSL context loaded (cert=" + config.cert_path + ")");
            } catch (std::exception& e) {
                Log::error(std::string("SSL init failed: ") + e.what());
                return 1;
            }
        }

        boost::asio::io_context ioc;
        auto bind_address = boost::asio::ip::make_address(config.bind_address);
        tcp::acceptor acceptor(ioc, tcp::endpoint(bind_address, config.port));

        auto limiter = std::make_shared<ConnectionLimiter>(
            config.max_connections,
            config.max_connections_per_ip
        );

        ApiAuthConfig auth_config;
        auth_config.api_base_url      = config.auth_api_url;
        auth_config.api_secret_key    = config.auth_api_secret;
        auth_config.cache_ttl_seconds = config.auth_cache_ttl;
        auth_config.timeout_seconds   = config.auth_api_timeout;
        auto auth_manager = std::make_shared<AuthManager>(auth_config);

        std::shared_ptr<MQTTPublisher> mqtt_publisher;
        if (!config.mqtt_url.empty()) {
            auto mqtt_cfg = MQTTConfig::fromUrl(config.mqtt_url, config.mqtt_topic, config.mqtt_client_id);
            mqtt_publisher = std::make_shared<MQTTPublisher>(mqtt_cfg);
            if (mqtt_publisher->connect()) {
                Log::info("MQTT: connecting to " + config.mqtt_url + " topic=" + config.mqtt_topic);
            } else {
                Log::warn("MQTT: connect() failed, publishing disabled");
                mqtt_publisher.reset();
            }
        } else {
            Log::info("MQTT: disabled");
        }

        Log::info("Listening on " + std::string(use_ssl ? "wss" : "ws") +
                  "://" + config.bind_address + ":" + std::to_string(config.port));

        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](boost::system::error_code const&, int) {
            Log::info("Received termination signal, performing graceful shutdown...");
            acceptor.close(); // Stop accepting new connections
            SessionTracker::instance().shutdownAll(); // Interrupt active sessions
            // The io_context will run out of work naturally if we wait briefly, or we can just exit
            std::this_thread::sleep_for(std::chrono::seconds(2)); // Give 2 seconds for buffers to flush
            std::exit(0);
        });

        while (acceptor.is_open()) {
            tcp::socket socket(ioc);
            boost::system::error_code ec;
            acceptor.accept(socket, ec);

            if (ec) {
                if (ec == boost::asio::error::operation_aborted) break;
                Log::error("Accept error: " + ec.message());
                continue;
            }

            std::string client_ip = socket.remote_endpoint().address().to_string();

            if (!limiter->tryAcquire(client_ip)) {
                Log::warn("Connection rejected (limit reached): " + client_ip);
                socket.close();
                continue;
            }

            Log::info("New connection from " + client_ip);

            try {
                std::thread(handleSession,
                            std::move(socket),
                            limiter,
                            client_ip,
                            config.model_path,
                            auth_manager,
                            config.whisper_beam_size,
                            config.whisper_threads,
                            config.whisper_initial_prompt,
                            config.session_timeout_sec,
                            ssl_ctx,
                            mqtt_publisher).detach();
            } catch (...) {
                limiter->release(client_ip);
                throw;
            }
        }
    } catch (std::exception& e) {
        Log::error(std::string("Fatal server error: ") + e.what());
        return 1;
    }

    return 0;
}
