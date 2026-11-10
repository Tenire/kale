#ifndef KALE_MIDDLEWARE_CORS_H
#define KALE_MIDDLEWARE_CORS_H

#include <string>
#include <vector>

#include "kale/core/router.h"

namespace kale {

struct CorsConfig {
    std::vector<std::string> allow_origins{"*"};
    std::vector<std::string> allow_methods{"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"};
    std::vector<std::string> allow_headers{"*"};
    std::vector<std::string> expose_headers;
    bool allow_credentials{false};
    int max_age{86400}; // Seconds; zero omits Access-Control-Max-Age.
};

// Handles preflight OPTIONS requests (204 when allowed, 403 when forbidden).
// For simple requests, attaches CORS headers and merges Vary: Origin before next().
Middleware cors(CorsConfig config = {});

} // namespace kale

#endif // KALE_MIDDLEWARE_CORS_H
