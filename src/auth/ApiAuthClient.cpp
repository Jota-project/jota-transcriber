#include "ApiAuthClient.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <stdexcept>
#include <regex>

#include "log/Log.h"

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
namespace ssl   = asio::ssl;
using     tcp   = asio::ip::tcp;

namespace {

bool parseUrl(const std::string& url,
              std::string& scheme,
              std::string& host,
              std::string& base_path) {
    static const std::regex re(R"(^(https?)://([^/]+)(/.*)?)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) {
        return false;
    }
    scheme    = m[1].str();
    host      = m[2].str();
    base_path = m[3].matched ? m[3].str() : "";
    return true;
}

// Returns the first non-link-local endpoint for the given host:port.
tcp::endpoint resolveEndpoint(tcp::resolver& resolver,
                              const std::string& host,
                              const std::string& port) {
    auto const results = resolver.resolve(host, port);
    for (auto const& ep : results) {
        auto addr = ep.endpoint().address();
        if (addr.is_v6() && addr.to_v6().is_link_local()) continue;
        return ep.endpoint();
    }
    throw std::runtime_error("no routable address for " + host);
}

} // namespace

ApiAuthClient::ApiAuthClient(const ApiAuthConfig& config)
    : config_(config) {}

AuthResult ApiAuthClient::validate(const std::string& client_key) {
    std::string scheme, host, base_path;
    if (!parseUrl(config_.api_base_url, scheme, host, base_path)) {
        Log::error("Invalid auth API URL: " + config_.api_base_url);
        return AuthResult::ApiUnavailable;
    }

    const bool        use_tls = (scheme == "https");
    const std::string port    = use_tls ? "443" : "80";
    const std::string target  = base_path + "/client";
    const std::string masked  = Log::maskKey(client_key);

    Log::debug("Auth API GET " + config_.api_base_url + "/client key=" + masked);

    try {
        asio::io_context ioc;
        tcp::resolver    resolver(ioc);
        auto const       endpoint = resolveEndpoint(resolver, host, port);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "TranscriptionServer/1.0");
        req.set(http::field::authorization, "Bearer " + config_.api_secret_key);
        req.set("X-API-Key", client_key);

        beast::flat_buffer              buf;
        http::response<http::string_body> res;

        if (use_tls) {
            ssl::context ctx(ssl::context::tls_client);
            // Internal service — skip certificate verification.
            ctx.set_verify_mode(ssl::verify_none);

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
            // SNI
            SSL_set_tlsext_host_name(stream.native_handle(), host.c_str());

            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(config_.timeout_seconds));
            beast::get_lowest_layer(stream).connect(endpoint);

            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(config_.timeout_seconds));
            stream.handshake(ssl::stream_base::client);

            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(config_.timeout_seconds));
            http::write(stream, req);

            beast::get_lowest_layer(stream).expires_after(
                std::chrono::seconds(config_.timeout_seconds));
            http::read(stream, buf, res);

            beast::error_code ec;
            stream.shutdown(ec);
        } else {
            beast::tcp_stream stream(ioc);
            stream.expires_after(std::chrono::seconds(config_.timeout_seconds));
            stream.connect(endpoint);

            stream.expires_after(std::chrono::seconds(config_.timeout_seconds));
            http::write(stream, req);

            stream.expires_after(std::chrono::seconds(config_.timeout_seconds));
            http::read(stream, buf, res);

            beast::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
        }

        const auto status = res.result_int();
        Log::debug("Auth API response: HTTP " + std::to_string(status) + " key=" + masked);

        if (status == 401 || status == 403) {
            Log::info("Auth API: Denied (HTTP " + std::to_string(status) + ") key=" + masked);
            return AuthResult::Denied;
        }

        if (status == 200) {
            try {
                auto body   = nlohmann::json::parse(res.body());
                bool active = body.value("is_active", false);
                if (active) {
                    Log::info("Auth API: Allowed key=" + masked);
                    return AuthResult::Allowed;
                } else {
                    Log::info("Auth API: Denied (is_active=false) key=" + masked);
                    return AuthResult::Denied;
                }
            } catch (...) {
                Log::warn("Auth API returned HTTP 200 but invalid JSON body key=" + masked);
                return AuthResult::ApiUnavailable;
            }
        }

        Log::warn("Auth API unexpected HTTP " + std::to_string(status) + " key=" + masked);
        return AuthResult::ApiUnavailable;

    } catch (std::exception& e) {
        Log::error("Auth API request failed: " + std::string(e.what()) + " key=" + masked);
        return AuthResult::ApiUnavailable;
    }
}
