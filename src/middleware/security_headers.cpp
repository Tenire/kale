#include "kale/middleware/security_headers.h"

#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace kale {
namespace {
using Header = std::pair<std::string_view, std::string>;

bool safe_value(std::string_view value) noexcept {
    for (unsigned char byte : value) {
        if (byte < 32 || byte == 127) return false;
    }
    return true;
}
} // namespace

Middleware security_headers(SecurityHeadersConfig config) {
    std::array<Header, 10> headers{{
        {"X-Content-Type-Options", config.content_type_nosniff ? "nosniff" : ""},
        {"X-Frame-Options", std::move(config.frame_options)},
        {"X-XSS-Protection", std::move(config.xss_protection)},
        {"Strict-Transport-Security", std::move(config.hsts)},
        {"Referrer-Policy", std::move(config.referrer_policy)},
        {"Content-Security-Policy", std::move(config.content_security_policy)},
        {"Permissions-Policy", std::move(config.permissions_policy)},
        {"Cross-Origin-Opener-Policy", std::move(config.cross_origin_opener_policy)},
        {"Cross-Origin-Resource-Policy", std::move(config.cross_origin_resource_policy)},
        {"Cross-Origin-Embedder-Policy", std::move(config.cross_origin_embedder_policy)}
    }};
    for (const auto &header : headers) {
        if (!safe_value(header.second)) throw std::invalid_argument("invalid security header value");
    }
    return [headers = std::move(headers)](Context &ctx, Next next) -> coke::Task<void> {
        for (const auto &[name, value] : headers) {
            if (!value.empty() && !ctx.has_resp_header(name)) ctx.set_header(name, value);
        }
        co_await next();
    };
}

} // namespace kale
