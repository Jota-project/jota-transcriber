#include <gtest/gtest.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "server/SessionHandler.h"
#include "server/HttpRouter.h"
#include "server/ConnectionLimiter.h"
#include "server/AuthManager.h"
#include "auth/ApiAuthConfig.h"
#include "server/ServerConfig.h"
#include <chrono>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace {

// A router with a single POST /slow route whose handler sleeps past the
// configured handshake timeout before responding — standing in for a
// legitimately slow synchronous HTTP handler (e.g. a long whisper
// transcription in HandleTranscribe).
HttpRouter buildSlowRouter(std::chrono::milliseconds handler_delay) {
    HttpRouter router;
    router.add(http::verb::post, "/slow",
        [handler_delay](const http::request<http::string_body>& req, SendFn send,
                        const ServerConfig&, const std::shared_ptr<AuthManager>&) {
            std::this_thread::sleep_for(handler_delay);
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "text/plain");
            res.body() = "done";
            res.prepare_payload();
            send(res);
        });
    return router;
}

} // namespace

// jota-transcriber#81: HandshakeWatchdog is meant to bound only the TLS
// handshake + initial HTTP read (a client trickling data slowly), but it
// stayed armed through synchronous handler execution too. Any request whose
// handler takes longer than --handshake-timeout-sec got its socket force-
// shut mid-response, and the client received nothing.
TEST(SessionHandler, SlowHandlerSurvivesHandshakeTimeout) {
    net::io_context ioc;
    tcp::acceptor acceptor(ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0));
    uint16_t port = acceptor.local_endpoint().port();

    ServerConfig config;
    config.handshake_timeout_sec = 1;                       // watchdog deadline
    const auto handler_delay = std::chrono::milliseconds(2000); // exceeds it

    HttpRouter router = buildSlowRouter(handler_delay);
    auto limiter = std::make_shared<ConnectionLimiter>(8, 2);
    ApiAuthConfig auth_cfg; // no token/API → auth disabled
    auto auth_manager = std::make_shared<AuthManager>(auth_cfg);

    std::thread server_thread([&]() {
        tcp::socket socket(ioc);
        acceptor.accept(socket);
        SessionHandler::handleSession(std::move(socket), limiter, "127.0.0.1",
                                       config, auth_manager, /*ssl_ctx=*/nullptr, router);
    });

    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve("127.0.0.1", std::to_string(port));
    tcp::socket client_socket(ioc);
    net::connect(client_socket, results.begin(), results.end());

    http::request<http::string_body> req{http::verb::post, "/slow", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "text/plain");
    req.body() = "x";
    req.prepare_payload();

    auto start = std::chrono::steady_clock::now();
    http::write(client_socket, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    boost::system::error_code ec;
    http::read(client_socket, buffer, res, ec);
    auto elapsed = std::chrono::steady_clock::now() - start;

    server_thread.join();

    ASSERT_FALSE(ec) << "client never received a response (ec=" << ec.message()
                      << ") — the handshake watchdog likely force-closed the "
                         "socket while the handler was still running";
    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_EQ(res.body(), "done");
    EXPECT_GE(elapsed, handler_delay)
        << "response arrived before the handler's artificial delay finished";
}
