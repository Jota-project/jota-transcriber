#pragma once
#include "server/HttpRouter.h"

namespace HandleHealth {
    void handle(const http::request<http::string_body>& req,
                SendFn send,
                const ServerConfig& config,
                const std::shared_ptr<AuthManager>& auth);
}