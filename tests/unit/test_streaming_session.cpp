#include <gtest/gtest.h>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include "server/StreamingSession.h"
#include "server/AuthManager.h"
#include "auth/ApiAuthConfig.h"
#include "server/SessionTracker.h"
#include "whisper/ModelCache.h"
#include "whisper/InferenceLimiter.h"
#include <thread>
#include <memory>
#include <atomic>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

class WsTestClient {
public:
    WsTestClient(net::io_context& ioc, uint16_t port)
        : resolver_(ioc), ws_(ioc) 
    {
        auto const results = resolver_.resolve("127.0.0.1", std::to_string(port));
        net::connect(ws_.next_layer(), results.begin(), results.end());
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req)
            {
                req.set(http::field::user_agent, "WsTestClient");
            }));
        ws_.handshake("127.0.0.1:" + std::to_string(port), "/");
    }

    void sendJson(const json& msg) {
        ws_.text(true);
        ws_.write(net::buffer(msg.dump()));
    }

    void sendBinary(const std::vector<unsigned char>& data) {
        ws_.binary(true);
        ws_.write(net::buffer(data));
    }

    json recvJson() {
        beast::flat_buffer buffer;
        ws_.read(buffer);
        std::string s(boost::asio::buffers_begin(buffer.data()), boost::asio::buffers_end(buffer.data()));
        return json::parse(s);
    }

    void close() {
        boost::system::error_code ec;
        ws_.close(websocket::close_code::normal, ec);
    }

    ~WsTestClient() {
        if (ws_.is_open()) {
            close();
        }
    }

private:
    tcp::resolver resolver_;
    websocket::stream<tcp::socket> ws_;
};

class StreamingSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        InferenceLimiter::instance().setMaxConcurrency(1);
        ModelCache::instance().configure(60);
    }

    static void TearDownTestSuite() {
        ModelCache::instance().forceUnload();
    }


    void TearDown() override {
        // Stop acceptor and IO context
        if (acceptor_ && acceptor_->is_open()) {
            boost::system::error_code ec;
            acceptor_->close(ec);
        }
        ioc_.stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        for (auto& t : session_threads_) {
            if (t.joinable()) t.join();
        }
    }

    uint16_t startServer(bool require_auth = false) {
        tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), 0);
        acceptor_ = std::make_unique<tcp::acceptor>(ioc_, endpoint);
        uint16_t port = acceptor_->local_endpoint().port();

        ApiAuthConfig auth_cfg;
        if (require_auth) {
            auth_cfg.api_base_url = "http://fake-auth";
            auth_cfg.static_token = "valid-token";
        }
        auto auth_manager = std::make_shared<AuthManager>(auth_cfg);

        doAccept(auth_manager);

        server_thread_ = std::thread([this]() { ioc_.run(); });
        return port;
    }

    void doAccept(std::shared_ptr<AuthManager> auth_manager) {
        acceptor_->async_accept(
            [this, auth_manager](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    session_threads_.emplace_back(
                        [this, s = std::move(socket), auth_manager]() mutable {
                            try {
                                boost::beast::flat_buffer buffer;
                                boost::beast::http::request<boost::beast::http::string_body> req;
                                boost::beast::http::read(s, buffer, req);

                                std::string model_path = std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/models/for-tests-ggml-tiny.bin";

                                websocket::stream<tcp::socket> ws(std::move(s));
                                auto session = std::make_shared<StreamingSession<websocket::stream<tcp::socket>>>(
                                    std::move(ws),
                                    model_path,
                                    auth_manager,
                                    5, 4, "", 30, 0.2f, 0.2f, 0.3f, -1.0f
                                );
                                session->run(req);
                            } catch (...) {}
                        });
                    doAccept(auth_manager);
                }
            });
    }

    WsTestClient connect(uint16_t port) {
        return WsTestClient(client_ioc_, port);
    }

private:
    net::io_context ioc_;
    net::io_context client_ioc_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::thread server_thread_;
    std::vector<std::thread> session_threads_;
};

TEST_F(StreamingSessionTest, BinaryBeforeConfigReturnsError) {
    auto port = startServer(false);
    auto client = connect(port);
    std::vector<unsigned char> someBytes = {1, 2, 3, 4, 1, 2, 3, 4}; // At least one float
    client.sendBinary(someBytes);
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "error");
    EXPECT_EQ(msg["code"], "NOT_CONFIGURED");
}

TEST_F(StreamingSessionTest, ConfigWithoutTokenWhenAuthActive) {
    auto port = startServer(true);
    auto client = connect(port);
    client.sendJson({{"type", "config"}, {"language", "es"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "error");
    EXPECT_EQ(msg["code"], "AUTH_REQUIRED");
}

TEST_F(StreamingSessionTest, ConfigWithInvalidToken) {
    auto port = startServer(true);
    auto client = connect(port);
    client.sendJson({{"type", "config"}, {"token", "wrong-token"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "error");
    EXPECT_EQ(msg["code"], "AUTH_FAILED");
}

TEST_F(StreamingSessionTest, ValidConfigReturnsReady) {
    auto port = startServer(false);
    auto client = connect(port);
    client.sendJson({{"type", "config"}, {"language", "es"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "ready");
    EXPECT_TRUE(msg.contains("session_id"));
    EXPECT_EQ(msg["config"]["language"], "es");
}

TEST_F(StreamingSessionTest, JsonWithoutType) {
    auto port = startServer(false);
    auto client = connect(port);
    client.sendJson({{"foo", "bar"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "error");
    EXPECT_EQ(msg["code"], "INVALID_MESSAGE");
}

TEST_F(StreamingSessionTest, UnknownType) {
    auto port = startServer(false);
    auto client = connect(port);
    
    // Config first to be in configured state
    client.sendJson({{"type", "config"}, {"language", "es"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "ready");

    client.sendJson({{"type", "unknown_foo"}});
    auto msg2 = client.recvJson();
    EXPECT_EQ(msg2["type"], "error");
    EXPECT_EQ(msg2["code"], "UNKNOWN_TYPE");
}

TEST_F(StreamingSessionTest, BinaryFrameTooLarge) {
    auto port = startServer(false);
    auto client = connect(port);
    // Config first so it doesn't fail on NOT_CONFIGURED
    client.sendJson({{"type", "config"}, {"language", "es"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "ready");

    // Send 1MB + 4 bytes
    std::vector<unsigned char> hugeData(1 * 1024 * 1024 + 4, 0);
    client.sendBinary(hugeData);
    
    // Server should close the connection with policy_error
    // Next read should throw an exception closing the WS
    EXPECT_ANY_THROW(client.recvJson()); 
}

TEST_F(StreamingSessionTest, EndWithoutAudio) {
    auto port = startServer(false);
    auto client = connect(port);
    
    client.sendJson({{"type", "config"}, {"language", "es"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "ready");

    // send end
    client.sendJson({{"type", "end"}});
    
    // We expect a final transcription msg
    auto trans = client.recvJson();
    EXPECT_EQ(trans["type"], "transcription");
    EXPECT_TRUE(trans["is_final"]);
    EXPECT_EQ(trans["text"], ""); // Empty since no audio
}

TEST_F(StreamingSessionTest, DoubleConfig) {
    auto port = startServer(false);
    auto client = connect(port);
    
    client.sendJson({{"type", "config"}, {"language", "es"}});
    auto msg = client.recvJson();
    EXPECT_EQ(msg["type"], "ready");

    // send config again
    client.sendJson({{"type", "config"}, {"language", "en"}});
    auto msg2 = client.recvJson();
    EXPECT_EQ(msg2["type"], "ready");
    EXPECT_EQ(msg2["config"]["language"], "en");
}
