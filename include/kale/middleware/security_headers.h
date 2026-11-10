#ifndef KALE_MIDDLEWARE_SECURITY_HEADERS_H
#define KALE_MIDDLEWARE_SECURITY_HEADERS_H

#include <string>

#include "kale/core/router.h"

namespace kale {

struct SecurityHeadersConfig {
    bool content_type_nosniff{true};
    std::string frame_options{"DENY"};
    std::string xss_protection{"0"};
    // Enable HSTS explicitly only for a deployment served over HTTPS.
    std::string hsts;
    std::string referrer_policy{"strict-origin-when-cross-origin"};
    std::string content_security_policy;
    std::string permissions_policy;
    std::string cross_origin_opener_policy{"same-origin"};
    std::string cross_origin_resource_policy{"same-origin"};
    std::string cross_origin_embedder_policy;
};

// Applies missing configured security headers to responses before next().
// Empty configuration values disable their corresponding headers.
Middleware security_headers(SecurityHeadersConfig config = {});

} // namespace kale

#endif // KALE_MIDDLEWARE_SECURITY_HEADERS_H
