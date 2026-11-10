#include "kale/middleware/cors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>

#include "kale/core/error_response.h"
#include "kale/core/http_error.h"
#include "kale/core/http_method.h"
#include "kale/core/http_util.h"
#include "workflow/HttpUtil.h"

namespace kale {
namespace {

std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == 32 || value.front() == 9)) value.remove_prefix(1);
    while (!value.empty() && (value.back() == 32 || value.back() == 9)) value.remove_suffix(1);
    return value;
}

bool ascii_alpha(unsigned char c) noexcept {
    return (c >= 65 && c <= 90) || (c >= 97 && c <= 122);
}

bool ascii_digit(unsigned char c) noexcept { return c >= 48 && c <= 57; }

bool valid_origin(std::string_view origin) noexcept {
    if (origin == "null") return true;
    const auto separator = origin.find("://");
    if (separator == std::string_view::npos || separator == 0 || !ascii_alpha(origin.front())) return false;
    for (unsigned char c : origin.substr(0, separator)) {
        if (!ascii_alpha(c) && !ascii_digit(c) && c != 43 && c != 45 && c != 46) return false;
    }
    auto authority = origin.substr(separator + 3);
    if (authority.empty()) return false;
    std::string_view port;
    if (authority.front() == 91) {
        const auto close = authority.find(93);
        if (close == std::string_view::npos) return false;
        auto address = authority.substr(1, close - 1);
        if (address.empty() || address.size() >= INET6_ADDRSTRLEN) return false;
        std::array<char, INET6_ADDRSTRLEN> buffer{};
        std::copy(address.begin(), address.end(), buffer.begin());
        in6_addr parsed{};
        if (::inet_pton(AF_INET6, buffer.data(), &parsed) != 1) return false;
        authority.remove_prefix(close + 1);
        if (!authority.empty()) {
            if (authority.front() != 58) return false;
            port = authority.substr(1);
            if (port.empty()) return false;
        }
    } else {
        const auto colon = authority.find(58);
        auto host = authority.substr(0, colon);
        if (host.empty() || host.size() > 253) return false;
        // Browser origins use ASCII/IDNA hosts, not userinfo, paths or whitespace.
        if (host.back() == 46) host.remove_suffix(1);
        if (host.empty()) return false;
        while (!host.empty()) {
            const auto dot = host.find(46);
            const auto label = host.substr(0, dot);
            if (label.empty() || label.size() > 63 || label.front() == 45 || label.back() == 45) return false;
            for (unsigned char c : label) {
                if (!ascii_alpha(c) && !ascii_digit(c) && c != 45) return false;
            }
            if (dot == std::string_view::npos) break;
            host.remove_prefix(dot + 1);
            if (host.empty()) return false;
        }
        if (colon != std::string_view::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) return false;
        }
    }
    if (!port.empty()) {
        for (unsigned char c : port) if (!ascii_digit(c)) return false;
        unsigned int number = 0;
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || number > 65535) return false;
    }
    return true;
}

bool single_header(const Context &ctx, std::string_view name, std::string_view &value) noexcept {
    protocol::HttpMessageHeader header;
    header.name = name.data();
    header.name_len = name.size();
    protocol::HttpHeaderCursor cursor(&ctx.raw_req());
    if (!cursor.find(&header)) return false;
    value = {static_cast<const char *>(header.value), header.value_len};
    return !cursor.find(&header);
}

bool contains_token(std::string_view list, std::string_view token) noexcept {
    while (!list.empty()) {
        const auto comma = list.find(44);
        const auto item = trim_ows(list.substr(0, comma));
        if (item == "*" || case_insensitive_equal(item, token)) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

void merge_vary(Context &ctx, std::string_view token) {
    protocol::HttpMessageHeader header;
    header.name = "Vary";
    header.name_len = 4;
    protocol::HttpHeaderCursor cursor(&ctx.raw_resp());
    while (cursor.find(&header)) {
        if (contains_token({static_cast<const char *>(header.value), header.value_len}, token)) return;
    }
    std::string value;
    protocol::HttpHeaderCursor all(&ctx.raw_resp());
    header.name = "Vary";
    header.name_len = 4;
    while (all.find(&header)) {
        const auto existing = trim_ows({static_cast<const char *>(header.value), header.value_len});
        if (!existing.empty()) {
            if (!value.empty()) value += ", ";
            value.append(existing);
        }
    }
    if (!value.empty()) value += ", ";
    value.append(token);
    ctx.erase_resp_header("Vary");
    ctx.set_header("Vary", value);
}

std::string join(const std::vector<std::string> &items) {
    std::size_t size = items.empty() ? 0 : (items.size() - 1) * 2;
    for (const auto &item : items) size += item.size();
    std::string result;
    result.reserve(size);
    for (const auto &item : items) {
        if (!result.empty()) result += ", ";
        result += item;
    }
    return result;
}

bool wildcard(const std::vector<std::string> &values) noexcept {
    return values.size() == 1 && values.front() == "*";
}

void validate_tokens(const std::vector<std::string> &values, bool allow_wildcard) {
    for (const auto &value : values) {
        if (!is_valid_header_name(value) ||
            (value == "*" && (!allow_wildcard || values.size() != 1))) {
            throw std::invalid_argument("invalid CORS token configuration");
        }
    }
}

bool allowed_headers(std::string_view requested, const CorsConfig &config, std::string &result) {
    const bool any = wildcard(config.allow_headers);
    for (;;) {
        const auto comma = requested.find(44);
        const auto name = trim_ows(requested.substr(0, comma));
        if (!is_valid_header_name(name) || name == "*") return false;
        // Authorization must always be explicitly permitted, even for wildcard.
        const bool listed = std::any_of(config.allow_headers.begin(), config.allow_headers.end(),
            [name](const auto &allowed) { return case_insensitive_equal(name, allowed); });
        if (!listed && (!any || case_insensitive_equal(name, "Authorization"))) return false;
        if (!result.empty()) result += ", ";
        result.append(name);
        if (comma == std::string_view::npos) return true;
        requested.remove_prefix(comma + 1);
    }
}

void set_grants(Context &ctx, const CorsConfig &config, std::string_view origin,
                std::string_view exposed) {
    if (!ctx.has_resp_header("Access-Control-Allow-Origin")) {
        ctx.set_header("Access-Control-Allow-Origin", wildcard(config.allow_origins) ? "*" : origin);
    }
    if (config.allow_credentials && !ctx.has_resp_header("Access-Control-Allow-Credentials")) {
        ctx.set_header("Access-Control-Allow-Credentials", "true");
    }
    if (!exposed.empty() && !ctx.has_resp_header("Access-Control-Expose-Headers")) {
        ctx.set_header("Access-Control-Expose-Headers", exposed);
    }
}

} // namespace

Middleware cors(CorsConfig config) {
    for (const auto &origin : config.allow_origins) {
        if ((origin == "*" && config.allow_origins.size() != 1) ||
            (origin != "*" && !valid_origin(origin))) {
            throw std::invalid_argument("invalid CORS origin configuration");
        }
    }
    if (config.allow_credentials && wildcard(config.allow_origins)) {
        throw std::invalid_argument("credentialed CORS requires explicit origins");
    }
    if (config.max_age < 0) throw std::invalid_argument("negative CORS max_age");
    validate_tokens(config.allow_methods, false);
    validate_tokens(config.allow_headers, true);
    validate_tokens(config.expose_headers, true);
    auto methods = join(config.allow_methods);
    auto exposed = join(config.expose_headers);
    auto max_age = config.max_age == 0 ? std::string{} : std::to_string(config.max_age);
    return [config = std::move(config), methods = std::move(methods), exposed = std::move(exposed), max_age = std::move(max_age)](Context &ctx, Next next) -> coke::Task<void> {
        // Vary is also required on responses without Origin/with a denied origin
        // so shared caches cannot reuse a grant from a different request.
        merge_vary(ctx, "Origin");
        const bool preflight = ctx.method() == HttpMethod::OPTIONS && ctx.has_header("Origin") &&
                               ctx.has_header("Access-Control-Request-Method");
        if (preflight) {
            merge_vary(ctx, "Access-Control-Request-Method");
            merge_vary(ctx, "Access-Control-Request-Headers");
        }
        std::string_view origin;
        const bool allowed = single_header(ctx, "Origin", origin) && valid_origin(origin) &&
            (wildcard(config.allow_origins) ||
             std::find(config.allow_origins.begin(), config.allow_origins.end(), origin) != config.allow_origins.end());
        if (preflight) {
            std::string_view method;
            std::string headers;
            bool permitted = allowed && single_header(ctx, "Access-Control-Request-Method", method) &&
                is_valid_header_name(method) &&
                std::find(config.allow_methods.begin(), config.allow_methods.end(), method) != config.allow_methods.end();
            if (permitted && ctx.has_header("Access-Control-Request-Headers")) {
                const auto requested = ctx.header("Access-Control-Request-Headers");
                permitted = allowed_headers(requested, config, headers);
            }
            if (!permitted) {
                reply_error(ctx, HttpError::forbidden("CORS origin not allowed", "CORS_FORBIDDEN"));
                co_return;
            }
            set_grants(ctx, config, origin, {});
            if (!methods.empty() && !ctx.has_resp_header("Access-Control-Allow-Methods")) {
                ctx.set_header("Access-Control-Allow-Methods", methods);
            }
            if (!headers.empty() && !ctx.has_resp_header("Access-Control-Allow-Headers")) {
                ctx.set_header("Access-Control-Allow-Headers", headers);
            }
            if (!max_age.empty() && !ctx.has_resp_header("Access-Control-Max-Age")) {
                ctx.set_header("Access-Control-Max-Age", max_age);
            }
            ctx.set_status(204);
            ctx.body("");
            co_return;
        }
        if (allowed) set_grants(ctx, config, origin, exposed);
        co_await next();
        if (!ctx.is_replied() && !ctx.is_stream()) merge_vary(ctx, "Origin");
    };
}

} // namespace kale
