#include "HandleReady.h"
#include "server/ServerConfig.h"
#include "whisper/InferenceLimiter.h"
#include <boost/beast/version.hpp>

namespace http = boost::beast::http;

void HandleReady::handle(const http::request<http::string_body>& req,
                         SendFn send,
                         const ServerConfig&,
                         const std::shared_ptr<AuthManager>&) {
    bool busy = !InferenceLimiter::instance().hasCapacity();
    http::response<http::string_body> res;
    res.version(req.version());
    res.result(busy ? http::status::service_unavailable : http::status::ok);
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "application/json");
    res.body() = busy ? "{\"status\": \"busy\"}" : "{\"status\": \"ready\"}";
    res.prepare_payload();
    send(std::move(res));
}