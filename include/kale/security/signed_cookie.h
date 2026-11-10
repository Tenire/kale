#ifndef KALE_SIGNED_COOKIE_H
#define KALE_SIGNED_COOKIE_H

#include "kale/core/cookie.h"

#include <optional>
#include <string>
#include <string_view>

namespace kale {

class Context;

/**
 * Signs cookie value using HMAC-SHA256, returning "<value>.<hmac_base64url>".
 */
std::string sign_cookie_value(std::string_view val, std::string_view secret);

/**
 * Verifies HMAC-SHA256 signature and returns the raw cookie value.
 * @return Raw cookie value if valid; std::nullopt otherwise.
 */
std::optional<std::string> unsign_cookie_value(std::string_view signed_val, std::string_view secret);

/**
 * Sets an HMAC-signed cookie on the response.
 */
Context &set_signed_cookie(Context &ctx, Cookie cookie, std::string_view secret);

/**
 * Sets an HMAC-signed cookie on the response.
 */
Context &set_signed_cookie(Context &ctx, std::string name, std::string value, std::string_view secret);

/**
 * Retrieves and verifies an HMAC-signed cookie from the request.
 * @return Verified raw cookie value if present and valid; std::nullopt otherwise.
 */
std::optional<std::string> signed_cookie(const Context &ctx, std::string_view name, std::string_view secret);

} // namespace kale

#endif // KALE_SIGNED_COOKIE_H
