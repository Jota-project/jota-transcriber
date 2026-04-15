#pragma once
#include "server/HttpRouter.h"
#include "server/ConnectionLimiter.h"
#include <memory>

namespace HandleMetrics {
    // limiter is passed explicitly because it is per-server, not a singleton.
    void handle(const http::request<http::string_body>& req,
                SendFn send,
                const ServerConfig& config,
                const std::shared_ptr<AuthManager>& auth,
                const std::shared_ptr<ConnectionLimiter>& limiter);
}