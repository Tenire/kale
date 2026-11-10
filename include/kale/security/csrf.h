#ifndef KALE_CSRF_H
#define KALE_CSRF_H

#include "kale/core/cookie.h"
#include "kale/core/router.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace kale {

/**
 * Configuration for Cross-Site Request Forgery (CSRF) protection middleware
 * implementing the signed Double-Submit Cookie pattern.
 */
struct CsrfConfig {
    // "__Host-" prefix enforces Secure=true, Path="/", and empty Domain.
    std::string cookie_name{"_csrf"};
    // Minimum 16 bytes for HMAC-SHA256.
    std::string secret;
    std::string header_name{"X-CSRF-Token"};
    std::string form_field_name{"_csrf"};
    std::string path{"/"};
    std::string domain{""};
    std::chrono::seconds max_age{std::chrono::hours(24)};
    // Required if same_site is None or cookie_name uses "__Host-".
    bool secure{true};
    // When true, application must provision the token via an endpoint or template.
    bool http_only{false};
    Cookie::SameSite same_site{Cookie::SameSite::Lax};
    // Methods exempt from token validation.
    std::vector<std::string> safe_methods{"GET", "HEAD", "OPTIONS", "TRACE"};
    std::vector<std::string> skip_paths{};
};

/**
 * Signed Double-Submit Cookie CSRF protection.
 * Configured safe methods issue or refresh the cookie if missing. All other
 * methods require a matching token via header or form, failing with 403.
 */
Middleware csrf(CsrfConfig config);

/**
 * Generates an HMAC-signed CSRF token.
 */
std::string generate_csrf_token(std::string_view secret);

/**
 * Verifies an HMAC-signed CSRF token in constant time.
 */
bool verify_csrf_token(std::string_view token, std::string_view secret);
} // namespace kale

#endif // KALE_CSRF_H