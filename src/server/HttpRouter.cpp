#include "HttpRouter.h"

void HttpRouter::add(http::verb method, std::string path, HttpHandler handler) {
    routes_.push_back({method, std::move(path), std::move(handler)});
}

bool HttpRouter::dispatch(const http::request<http::string_body>& req,
                          SendFn send,
                          const ServerConfig& config,
                          const std::shared_ptr<AuthManager>& auth) const {
    for (const auto& route : routes_) {
        if (route.method == req.method() &&
            std::string(req.target()) == route.path) {
            route.handler(req, std::move(send), config, auth);
            return true;
        }
    }
    return false;
}