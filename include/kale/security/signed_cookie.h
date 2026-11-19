#ifndef KALE_SIGNED_COOKIE_H
#define KALE_SIGNED_COOKIE_H

#include "kale/core/cookie.h"

#include <chrono>
#include <cstdint>
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

/**
 * Signs cookie value with a Unix timestamp (seconds) using HMAC-SHA256,
 * returning "<value>.<issued_at_unix_sec>.<hmac_base64url>".
 * When issued_at_epoch_sec < 0, uses current system_clock Unix epoch seconds.
 */
std::string sign_cookie_value_with_ttl(
    std::string_view val,
    std::string_view secret,
    int64_t issued_at_epoch_sec = -1);

/**
 * Verifies HMAC-SHA256 signature and enforces age in [0, max_age.count()].
 * When now_epoch_sec < 0, uses current system_clock Unix epoch seconds.
 */
std::optional<std::string> unsign_cookie_value_with_ttl(
    std::string_view signed_val,
    std::string_view secret,
    std::chrono::seconds max_age,
    int64_t now_epoch_sec = -1);

/**
 * Sets a TTL-bound HMAC-signed cookie on the response and synchronizes Cookie::max_age.
 */
Context &set_signed_cookie_with_ttl(
    Context &ctx,
    Cookie cookie,
    std::string_view secret,
    std::chrono::seconds max_age);

Context &set_signed_cookie_with_ttl(
    Context &ctx,
    std::string name,
    std::string value,
    std::string_view secret,
    std::chrono::seconds max_age);

/**
 * Retrieves and verifies a TTL-bound HMAC-signed cookie from the request.
 */
std::optional<std::string> signed_cookie_with_ttl(
    const Context &ctx,
    std::string_view name,
    std::string_view secret,
    std::chrono::seconds max_age);

} // namespace kale

#endif // KALE_SIGNED_COOKIE_H
