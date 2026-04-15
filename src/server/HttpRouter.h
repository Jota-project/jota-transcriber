#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <boost/beast/http.hpp>

class AuthManager;
struct ServerConfig;

namespace http = boost::beast::http;

using SendFn = std::function<void(http::response<http::string_body>)>;

using HttpHandler = std::function<void(
    const http::request<http::string_body>&,
    SendFn,
    const ServerConfig&,
    const std::shared_ptr<AuthManager>&
)>;

class HttpRouter {
public:
    void add(http::verb method, std::string path, HttpHandler handler);

    // Returns true if a route matched and the handler was called.
    // Returns false if no route matches (caller should handle as WebSocket or 404).
    bool dispatch(const http::request<http::string_body>& req,
                  SendFn send,
                  const ServerConfig& config,
                  const std::shared_ptr<AuthManager>& auth) const;

private:
    struct Route {
        http::verb  method;
        std::string path;
        HttpHandler handler;
    };
    std::vector<Route> routes_;
};