#pragma once
#include <string>

struct ApiAuthConfig {
    std::string api_base_url;       // e.g. "http://internal-api:8080"
    std::string api_secret_key;     // Authorization: Bearer <...>
    int timeout_seconds = 5;
    int cache_ttl_seconds = 300;
};
