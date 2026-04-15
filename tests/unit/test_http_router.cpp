#include <gtest/gtest.h>
#include "server/HttpRouter.h"
#include "server/ServerConfig.h"

namespace http = boost::beast::http;

TEST(HttpRouterTest, DispatchesMatchingRoute) {
    HttpRouter router;
    bool called = false;
    router.add(http::verb::get, "/health",
        [&](const auto&, SendFn send, const ServerConfig&, const std::shared_ptr<AuthManager>&) {
            called = true;
            http::response<http::string_body> res;
            res.result(http::status::ok);
            res.body() = "ok";
            res.prepare_payload();
            send(std::move(res));
        });

    http::request<http::string_body> req{http::verb::get, "/health", 11};
    bool send_called = false;
    SendFn sendFn = [&](http::response<http::string_body>) { send_called = true; };

    EXPECT_TRUE(router.dispatch(req, sendFn, ServerConfig{}, nullptr));
    EXPECT_TRUE(called);
    EXPECT_TRUE(send_called);
}

TEST(HttpRouterTest, ReturnsFalseForUnknownPath) {
    HttpRouter router;
    http::request<http::string_body> req{http::verb::get, "/unknown", 11};
    EXPECT_FALSE(router.dispatch(req, [](auto){}, ServerConfig{}, nullptr));
}

TEST(HttpRouterTest, DoesNotMatchWrongMethod) {
    HttpRouter router;
    router.add(http::verb::post, "/transcribe",
        [](const auto&, SendFn, const ServerConfig&, const std::shared_ptr<AuthManager>&){});
    http::request<http::string_body> req{http::verb::get, "/transcribe", 11};
    EXPECT_FALSE(router.dispatch(req, [](auto){}, ServerConfig{}, nullptr));
}

TEST(HttpRouterTest, MatchesFirstRegisteredRoute) {
    HttpRouter router;
    int call_count = 0;
    auto h = [&](const auto&, SendFn, const ServerConfig&, const std::shared_ptr<AuthManager>&) {
        ++call_count;
    };
    router.add(http::verb::get, "/ping", h);
    router.add(http::verb::get, "/ping", h); // duplicate

    http::request<http::string_body> req{http::verb::get, "/ping", 11};
    router.dispatch(req, [](auto){}, ServerConfig{}, nullptr);
    EXPECT_EQ(call_count, 1); // only first match fires
}