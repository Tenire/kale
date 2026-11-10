#ifndef KALE_MIDDLEWARE_REQUEST_ID_H
#define KALE_MIDDLEWARE_REQUEST_ID_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "kale/core/router.h"

namespace kale {

struct RequestId {
    std::string value;
};

struct RequestIdConfig {
    std::string header_name{"X-Request-Id"};
    std::size_t max_length{128}; // Inclusive; supported range 1..4096 bytes.
    std::function<std::string()> generator;
};

// Generates a 128-bit pseudo-random identifier formatted as 32 lowercase hex characters.
std::string default_request_id_generator();

// Validates incoming request ID headers, generating replacements when invalid or absent.
// Attaches RequestId to context and sets the response header before next().
Middleware request_id(RequestIdConfig config = {});

// Returns the request ID attached to ctx, or an empty view if none exists.
inline std::string_view get_request_id(const Context &ctx) noexcept {
    const auto *id = ctx.get_attachment<RequestId>();
    return id ? std::string_view(id->value) : std::string_view{};
}

} // namespace kale

#endif // KALE_MIDDLEWARE_REQUEST_ID_H
