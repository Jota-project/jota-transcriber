#include "ApiAuthClient.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <stdexcept>
#include <regex>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
using     tcp   = asio::ip::tcp;

namespace {

// Parse "http://host:port" or "http://host" into (host, port, base_path).
// Returns false if parsing fails.
bool parseUrl(const std::string& url,
              std::string& host,
              std::string& port,
              std::string& base_path) {
    // Only plain HTTP supported (no TLS for internal API)
    static const std::regex re(R"(^http://([^/:]+)(?::(\d+))?(/.*)?)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) {
        return false;
    }
    host      = m[1].str();
    port      = m[2].matched ? m[2].str() : "80";
    base_path = m[3].matched ? m[3].str() : "";
    return true;
}

} // namespace

ApiAuthClient::ApiAuthClient(const ApiAuthConfig& config)
    : config_(config) {}

AuthResult ApiAuthClient::validate(const std::string& client_key) {
    try {
        std::string host, port, base_path;
        if (!parseUrl(config_.api_base_url, host, port, base_path)) {
            return AuthResult::ApiUnavailable;
        }

        const std::string target = base_path + "/client";

        asio::io_context ioc;
        tcp::resolver resolver(ioc);

        auto const results = resolver.resolve(host, port);

        beast::tcp_stream stream(ioc);
        stream.expires_after(std::chrono::seconds(config_.timeout_seconds));
        stream.connect(results);

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "TranscriptionServer/1.0");
        req.set(http::field::authorization, "Bearer " + config_.api_secret_key);
        req.set("X-API-Key", client_key);

        stream.expires_after(std::chrono::seconds(config_.timeout_seconds));
        http::write(stream, req);

        beast::flat_buffer buf;
        http::response<http::string_body> res;
        stream.expires_after(std::chrono::seconds(config_.timeout_seconds));
        http::read(stream, buf, res);

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        const auto status = res.result_int();

        if (status == 401 || status == 403) {
            return AuthResult::Denied;
        }

        if (status == 200) {
            try {
                auto body = nlohmann::json::parse(res.body());
                bool active = body.value("is_active", false);
                return active ? AuthResult::Allowed : AuthResult::Denied;
            } catch (...) {
                // Malformed JSON from API
                return AuthResult::ApiUnavailable;
            }
        }

        // Any other HTTP status (5xx, etc.) → treat as unavailable
        return AuthResult::ApiUnavailable;

    } catch (...) {
        return AuthResult::ApiUnavailable;
    }
}
