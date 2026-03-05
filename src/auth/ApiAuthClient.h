#pragma once
#include <string>
#include "ApiAuthConfig.h"

enum class AuthResult {
    Allowed,
    Denied,
    ApiUnavailable
};

class ApiAuthClient {
public:
    explicit ApiAuthClient(const ApiAuthConfig& config);

    AuthResult validate(const std::string& client_key);

private:
    ApiAuthConfig config_;
};
