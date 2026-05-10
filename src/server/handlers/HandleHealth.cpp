#include "HandleHealth.h"
#include "server/ServerConfig.h"
#include <boost/beast/version.hpp>

namespace http = boost::beast::http;

void HandleHealth::handle(const http::request<http::string_body>& req,
                          SendFn send,
                          const ServerConfig&,
                          const std::shared_ptr<AuthManager>&) {
    http::response<http::string_body> res;
    res.version(req.version());
    res.result(http::status::ok);
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, "application/json");
    res.body() = "{\"status\": \"ok\"}";
    res.prepare_payload();
    send(std::move(res));
}