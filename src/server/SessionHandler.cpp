#include "server/SessionHandler.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include "server/StreamingSession.h"
#include "server/ConnectionGuard.h"
#include "server/ConnectionLimiter.h"
#include "server/HandshakeWatchdog.h"
#include "server/AuthManager.h"
#include "server/ServerConfig.h"
#include "log/Log.h"

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
namespace ssl = boost::asio::ssl;

namespace SessionHandler {

void handleSession(tcp::socket socket,
                   const std::shared_ptr<ConnectionLimiter>& limiter,
                   const std::string& client_ip,
                   const ServerConfig& config,
                   const std::shared_ptr<AuthManager>& auth_manager,
                   std::shared_ptr<ssl::context> ssl_ctx,
                   const HttpRouter& router) {
    ConnectionGuard guard(limiter, client_ip);

    // The ConnectionGuard slot above is held from here on. Before the
    // StreamingSession exists (and takes over via its own idle watchdog,
    // jota-transcriber#68), the TLS handshake and the initial HTTP upgrade
    // read below have no timeout at all otherwise: a client that opens a
    // TCP connection and sends nothing (or trickles data arbitrarily
    // slowly) would hold this slot forever. native_handle() is captured
    // before any std::move below — it's the same OS-level fd regardless of
    // which C++ wrapper currently owns it.
    HandshakeWatchdog handshake_watchdog(socket.native_handle(), config.handshake_timeout_sec);

    try {
        boost::beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(config.max_upload_bytes);

        auto send404 = [](auto& stream, const http::request<http::string_body>& req) {
            http::response<http::string_body> res{http::status::not_found, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = R"({"error":{"message":"Not found","type":"not_found"}})";
            res.prepare_payload();
            boost::beast::http::write(stream, res);
        };

        if (ssl_ctx) {
            ssl::stream<tcp::socket> ssl_stream(std::move(socket), *ssl_ctx);
            ssl_stream.handshake(ssl::stream_base::server);
            boost::beast::http::read(ssl_stream, buffer, parser);
            auto req = parser.release();
            // Full request (headers + body) is in hand — whatever happens
            // next (a synchronous handler or a WS upgrade) is legitimate
            // processing, not the "client trickling data slowly" case this
            // watchdog exists to guard against (jota-transcriber#81).
            handshake_watchdog.disarm();

            SendFn send = [&](http::response<http::string_body> res) {
                boost::beast::http::write(ssl_stream, res);
            };
            if (router.dispatch(req, send, config, auth_manager)) return;

            if (!websocket::is_upgrade(req)) {
                send404(ssl_stream, req);
                return;
            }

            websocket::stream<ssl::stream<tcp::socket>> ws(std::move(ssl_stream));
            auto session = std::make_shared<StreamingSession<websocket::stream<ssl::stream<tcp::socket>>>>(
                std::move(ws), config.model_path, auth_manager,
                config.whisper_beam_size, config.whisper_threads,
                config.whisper_initial_prompt, config.session_timeout_sec,
                config.whisper_temperature, config.whisper_temperature_inc,
                config.whisper_no_speech_thold, config.whisper_logprob_thold,
                config.flush_min_new_audio_ms,
                config.vad_model_path, config.vad_threshold,
                config.vad_min_speech_ms, config.vad_min_silence_ms,
                config.vad_max_speech_s, config.vad_speech_pad_ms,
                config.vad_samples_overlap);
            session->run(req);
        } else {
            boost::beast::http::read(socket, buffer, parser);
            auto req = parser.release();
            handshake_watchdog.disarm();

            SendFn send = [&](http::response<http::string_body> res) {
                boost::beast::http::write(socket, res);
            };
            if (router.dispatch(req, send, config, auth_manager)) return;

            if (!websocket::is_upgrade(req)) {
                send404(socket, req);
                return;
            }

            websocket::stream<tcp::socket> ws(std::move(socket));
            auto session = std::make_shared<StreamingSession<websocket::stream<tcp::socket>>>(
                std::move(ws), config.model_path, auth_manager,
                config.whisper_beam_size, config.whisper_threads,
                config.whisper_initial_prompt, config.session_timeout_sec,
                config.whisper_temperature, config.whisper_temperature_inc,
                config.whisper_no_speech_thold, config.whisper_logprob_thold,
                config.flush_min_new_audio_ms,
                config.vad_model_path, config.vad_threshold,
                config.vad_min_speech_ms, config.vad_min_silence_ms,
                config.vad_max_speech_s, config.vad_speech_pad_ms,
                config.vad_samples_overlap);
            session->run(req);
        }
    } catch (std::exception& e) {
        Log::error("Session exception from " + client_ip + ": " + e.what());
    }
}

} // namespace SessionHandler
