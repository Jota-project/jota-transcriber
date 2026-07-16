#include <gtest/gtest.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
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
namespace websocket = boost::beast::websocket;
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
    ApiAuthConfig auth_cfg; // handleSession() requires a non-null AuthManager;
                             // the /slow route never consults it, so its
                             // enabled/disabled state is irrelevant here
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

// jota-transcriber#81 follow-up: the refactor made handleSession()'s
// WebSocket-upgrade branch reachable from a test for the first time, but
// only the non-upgrading HTTP path got one (above). This exercises the
// is_upgrade branch itself: a real WS handshake through handleSession(),
// followed by a wait past handshake_timeout_sec with no application traffic
// (mirroring a client that's slow to send its first "config" message) to
// prove the watchdog stays disarmed after handoff into StreamingSession —
// not just for the synchronous-HTTP case.
TEST(SessionHandler, WebSocketUpgradeSurvivesHandshakeTimeoutAfterHandoff) {
    net::io_context ioc;
    tcp::acceptor acceptor(ioc, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0));
    uint16_t port = acceptor.local_endpoint().port();

    ServerConfig config;
    config.handshake_timeout_sec = 1;                          // watchdog deadline
    const auto post_upgrade_idle = std::chrono::milliseconds(1500); // exceeds it

    HttpRouter router; // no HTTP routes registered — this request must fall through to WS upgrade
    auto limiter = std::make_shared<ConnectionLimiter>(8, 2);
    ApiAuthConfig auth_cfg; // handleSession() requires a non-null AuthManager;
                             // no "config" message is ever sent in this test,
                             // so its enabled/disabled state is irrelevant
    auto auth_manager = std::make_shared<AuthManager>(auth_cfg);

    std::thread server_thread([&]() {
        tcp::socket socket(ioc);
        acceptor.accept(socket);
        SessionHandler::handleSession(std::move(socket), limiter, "127.0.0.1",
                                       config, auth_manager, /*ssl_ctx=*/nullptr, router);
    });

    tcp::resolver resolver(ioc);
    auto const results = resolver.resolve("127.0.0.1", std::to_string(port));
    websocket::stream<tcp::socket> client_ws(ioc);
    net::connect(client_ws.next_layer(), results.begin(), results.end());

    boost::system::error_code ec;
    client_ws.handshake("127.0.0.1", "/", ec);
    ASSERT_FALSE(ec) << "WS handshake through handleSession() failed: " << ec.message();

    // No "config", no audio — just sit on the open connection past the
    // watchdog's deadline. If handleSession() ever regressed to disarming
    // only after router.dispatch() (the pre-#81 behavior) or re-armed the
    // watchdog anywhere on this branch, the fd would be force-shut here and
    // the close handshake below would fail with a reset/broken-pipe error.
    std::this_thread::sleep_for(post_upgrade_idle);

    client_ws.close(websocket::close_code::normal, ec);

    server_thread.join();

    EXPECT_FALSE(ec) << "clean WS close failed (ec=" << ec.message()
                      << ") — the handshake watchdog likely force-closed the "
                         "socket after upgrade, before the idle client could "
                         "send anything";
}
