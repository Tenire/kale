#include "kale/security/csrf.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"

#include "kale/core/cookie.h"
#include "kale/http/form.h"
#include "kale/core/http_status.h"
#include "kale/core/http_util.h"
#include "http/base64.h"
#include "security/crypto_util.h"

#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace kale {

std::string generate_csrf_token(std::string_view secret) {
    if (secret.empty() || secret.size() < 16) {
        throw std::invalid_argument("generate_csrf_token: secret must be at least 16 bytes");
    }
    unsigned char raw_rand[32];
    if (!CryptoUtil::random_bytes(raw_rand, sizeof(raw_rand))) {
        throw std::runtime_error("generate_csrf_token: CSPRNG failed");
    }
    std::string entropy = Base64::encode_url(raw_rand, sizeof(raw_rand));
    std::string sig = CryptoUtil::hmac_sha256_base64url(entropy, secret);
    return entropy + "." + sig;
}

bool verify_csrf_token(std::string_view token, std::string_view secret) {
    if (secret.empty() || secret.size() < 16) {
        return false;
    }
    size_t dot_pos = token.find('.');
    if (dot_pos == std::string_view::npos || dot_pos == 0) {
        return false;
    }
    if (token.find('.', dot_pos + 1) != std::string_view::npos) {
        return false;
    }
    std::string_view entropy = token.substr(0, dot_pos);
    std::string_view given_sig = token.substr(dot_pos + 1);

    // 32-byte CSPRNG base64url is 43 chars, HMAC-SHA256 base64url is 43 chars
    if (entropy.size() != 43 || given_sig.size() != 43) {
        return false;
    }

    std::string expected_sig = CryptoUtil::hmac_sha256_base64url(entropy, secret);
    return CryptoUtil::constant_time_equals(given_sig, expected_sig);
}

Middleware csrf(CsrfConfig config) {
    if (config.secret.empty() || config.secret.size() < 16) {
        throw std::invalid_argument("CSRF: secret must be at least 16 bytes");
    }
    if (config.same_site == Cookie::SameSite::None && !config.secure) {
        throw std::invalid_argument("CSRF: SameSite=None requires Secure=true");
    }
    if (config.cookie_name.empty()) {
        throw std::invalid_argument("CSRF: cookie_name must not be empty");
    }
    if (config.header_name.empty()) {
        throw std::invalid_argument("CSRF: header_name must not be empty");
    }

    // Enforce RFC 6265bis __Host- prefix requirements
    if (config.cookie_name.starts_with("__Host-")) {
        if (!config.secure) {
            throw std::invalid_argument("CSRF: __Host- prefix requires Secure=true");
        }
        if (config.path != "/") {
            throw std::invalid_argument("CSRF: __Host- prefix requires Path=/");
        }
        if (!config.domain.empty()) {
            throw std::invalid_argument("CSRF: __Host- prefix requires empty Domain (host-only)");
        }
    }

    return [cfg = std::move(config)](Context &ctx, Next next) -> coke::Task<void> {
        for (const auto &skip : cfg.skip_paths) {
            if (ctx.path() == skip) {
                co_await next();
                co_return;
            }
        }

        // Detect duplicate headers and extract CSRF tokens
        int csrf_header_count = 0;
        std::string submitted_header_token;
        std::vector<std::string> cookie_tokens;

        protocol::HttpHeaderCursor cursor(&ctx.raw_req());
        std::string hname, hval;
        while (cursor.next(hname, hval)) {
            if (case_insensitive_equal(hname, cfg.header_name)) {
                csrf_header_count++;
                if (csrf_header_count == 1) {
                    submitted_header_token = hval;
                }
            } else if (case_insensitive_equal(hname, "Cookie")) {
                std::string_view cookie_hdr = hval;
                while (!cookie_hdr.empty()) {
                    auto semi = cookie_hdr.find(';');
                    std::string_view pair = (semi == std::string_view::npos) ? cookie_hdr : cookie_hdr.substr(0, semi);
                    if (semi == std::string_view::npos) cookie_hdr = {};
                    else cookie_hdr.remove_prefix(semi + 1);

                    while (!pair.empty() && (pair.front() == ' ' || pair.front() == '\t')) pair.remove_prefix(1);
                    while (!pair.empty() && (pair.back() == ' ' || pair.back() == '\t')) pair.remove_suffix(1);

                    auto eq = pair.find('=');
                    if (eq != std::string_view::npos) {
                        std::string_view cname = pair.substr(0, eq);
                        while (!cname.empty() && (cname.back() == ' ' || cname.back() == '\t')) cname.remove_suffix(1);
                        if (cname == cfg.cookie_name) {
                            std::string_view cval = pair.substr(eq + 1);
                            while (!cval.empty() && (cval.front() == ' ' || cval.front() == '\t')) cval.remove_prefix(1);
                            while (!cval.empty() && (cval.back() == ' ' || cval.back() == '\t')) cval.remove_suffix(1);
                            cookie_tokens.emplace_back(cval);
                        }
                    }
                }
            }
        }

        auto handle_forbidden = [&]() {
            reply_error(ctx, HttpError::forbidden("Invalid or missing CSRF token", "CSRF_FAILED"));
        };

        bool is_safe = false;
        for (const auto &m : cfg.safe_methods) {
            if (ctx.method_string() == m) {
                is_safe = true;
                break;
            }
        }

        if (is_safe) {
            // Retain existing valid cookie on safe requests
            bool has_valid_cookie = false;
            if (cookie_tokens.size() == 1 && verify_csrf_token(cookie_tokens[0], cfg.secret)) {
                has_valid_cookie = true;
            }

            if (!has_valid_cookie) {
                std::string token = generate_csrf_token(cfg.secret);
                Cookie c(cfg.cookie_name, token);
                c.set_path(cfg.path.empty() ? "/" : cfg.path);
                if (!cfg.domain.empty()) {
                    c.set_domain(cfg.domain);
                }
                c.set_max_age(static_cast<int>(cfg.max_age.count()));
                c.set_secure(cfg.secure);
                c.set_http_only(cfg.http_only);
                c.set_same_site(cfg.same_site);
                ctx.set_cookie(std::move(c));
            }

            co_await next();
            co_return;
        }

        // State-changing methods require a single valid cookie token
        if (cookie_tokens.size() != 1 || !verify_csrf_token(cookie_tokens[0], cfg.secret)) {
            handle_forbidden();
            co_return;
        }
        const std::string &cookie_token = cookie_tokens[0];

        if (csrf_header_count > 1) {
            handle_forbidden();
            co_return;
        }

        std::string form_token;
        bool has_form_token = false;
        std::string_view ct = ctx.content_type();
        if (ct.starts_with("application/x-www-form-urlencoded") || ct.starts_with("multipart/form-data")) {
            if (has_form(ctx, cfg.form_field_name)) {
                form_token = form(ctx, cfg.form_field_name);
                has_form_token = true;
            }
        }

        // Reject conflicting header and form tokens
        std::string submitted_token;
        if (csrf_header_count == 1 && has_form_token) {
            if (submitted_header_token != form_token) {
                handle_forbidden();
                co_return;
            }
            submitted_token = submitted_header_token;
        } else if (csrf_header_count == 1) {
            submitted_token = submitted_header_token;
        } else if (has_form_token) {
            submitted_token = form_token;
        } else {
            handle_forbidden();
            co_return;
        }

        // Constant-time token verification
        if (submitted_token.empty() ||
            !verify_csrf_token(submitted_token, cfg.secret) ||
            !CryptoUtil::constant_time_equals(submitted_token, cookie_token)) {
            handle_forbidden();
            co_return;
        }

        co_await next();
    };
}

} // namespace kale