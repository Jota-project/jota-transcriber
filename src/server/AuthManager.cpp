#include "AuthManager.h"

AuthManager::AuthManager(const ApiAuthConfig& config)
    : auth_enabled_(!config.api_base_url.empty()),
      cache_ttl_seconds_(config.cache_ttl_seconds) {
    if (auth_enabled_) {
        api_client_ = std::make_unique<ApiAuthClient>(config);
    }
}

bool AuthManager::isAuthEnabled() const {
    return auth_enabled_;
}

bool AuthManager::validate(const std::string& token) {
    if (!auth_enabled_) {
        return true;
    }

    // Cache hit?
    auto cached = cache_.get(token);
    if (cached.has_value()) {
        return cached.value();
    }

    // Call auth API
    AuthResult result = api_client_->validate(token);

    if (result == AuthResult::ApiUnavailable) {
        // Fail-closed: do not cache
        return false;
    }

    bool allowed = (result == AuthResult::Allowed);
    cache_.put(token, allowed, cache_ttl_seconds_);
    return allowed;
}
