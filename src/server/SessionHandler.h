#pragma once
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include "server/HttpRouter.h"

class ConnectionLimiter;

namespace SessionHandler {
    // Reads one HTTP request from `socket` and either dispatches it to a
    // matching route in `router` (health/ready/metrics/transcribe — all
    // synchronous) or, if it's a WebSocket upgrade, hands the connection
    // off to a long-lived StreamingSession. `router` is built once by the
    // caller and shared read-only across connections. Catches and logs all
    // exceptions; never throws.
    void handleSession(boost::asio::ip::tcp::socket socket,
                       const std::shared_ptr<ConnectionLimiter>& limiter,
                       const std::string& client_ip,
                       const ServerConfig& config,
                       const std::shared_ptr<AuthManager>& auth_manager,
                       std::shared_ptr<boost::asio::ssl::context> ssl_ctx,
                       const HttpRouter& router);
}
