#include "HandleMetrics.h"
#include "server/ServerConfig.h"
#include "whisper/InferenceLimiter.h"
#include "whisper/ModelCache.h"
#include <boost/beast/version.hpp>

namespace http = boost::beast::http;

void HandleMetrics::handle(const http::request<http::string_body>& req,
                           SendFn send,
                           const ServerConfig&,
                           const std::shared_ptr<AuthManager>&,
                           const std::shared_ptr<ConnectionLimiter>& limiter) {
    std::string body =
        "# HELP transcription_active_inferences Number of concurrent inferences\n"
        "# TYPE transcription_active_inferences gauge\n" +
        InferenceLimiter::instance().getMetrics() +
        "# HELP transcription_model_loaded Whether the model is currently in memory (1=yes, 0=no)\n"
        "# TYPE transcription_model_loaded gauge\n" +
        ModelCache::instance().getMetrics() +
        "# HELP transcription_active_connections Number of active WebSocket connections\n"
        "# TYPE transcription_active_connections gauge\n" +
        limiter->getMetrics();

    http::response<http::string_body> res;
    res.version(req.version());
    res.result(http::status::ok);
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "text/plain; version=0.0.4");
    res.body() = body;
    res.prepare_payload();
    send(std::move(res));
}