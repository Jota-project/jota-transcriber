#include "AuthManager.h"
#include "log/Log.h"

AuthManager::AuthManager(const ApiAuthConfig& config)
    : auth_enabled_(!config.api_base_url.empty()),
      cache_ttl_seconds_(config.cache_ttl_seconds) {
    if (auth_enabled_) {
        api_client_ = std::make_unique<ApiAuthClient>(config);
        Log::info("Auth enabled (API: " + config.api_base_url +
                  ", cache TTL: " + std::to_string(config.cache_ttl_seconds) + "s)");
    } else {
        Log::info("Auth disabled");
    }
}

bool AuthManager::isAuthEnabled() const {
    return auth_enabled_;
}

bool AuthManager::validate(const std::string& token) {
    if (!auth_enabled_) {
        return true;
    }

    const std::string masked = Log::maskKey(token);

    // Cache hit?
    auto cached = cache_.get(token);
    if (cached.has_value()) {
        Log::debug("Auth cache hit key=" + masked + " valid=" + (cached.value() ? "true" : "false"));
        return cached.value();
    }

    Log::debug("Auth cache miss key=" + masked + ", querying API");
    AuthResult result = api_client_->validate(token);

    if (result == AuthResult::ApiUnavailable) {
        Log::warn("Auth API unavailable for key=" + masked + ", denying (fail-closed)");
        return false;
    }

    bool allowed = (result == AuthResult::Allowed);
    Log::debug("Caching auth result key=" + masked +
               " allowed=" + (allowed ? "true" : "false") +
               " ttl=" + std::to_string(cache_ttl_seconds_) + "s");
    cache_.put(token, allowed, cache_ttl_seconds_);
    return allowed;
}
