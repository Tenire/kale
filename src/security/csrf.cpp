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
namespace {

std::string_view trim_ows(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) {
        sv.remove_suffix(1);
    }
    return sv;
}

std::string ascii_tolower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool has_invalid_authority_chars(std::string_view auth) {
    for (char c : auth) {
        if (c == '@' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            return true;
        }
    }
    return false;
}

std::string extract_authority_only(std::string_view url_or_host) {
    std::string_view sv = trim_ows(url_or_host);
    if (sv.empty()) {
        return "";
    }
    size_t scheme_pos = sv.find("://");
    std::string_view auth;
    if (scheme_pos != std::string_view::npos) {
        std::string_view rest = sv.substr(scheme_pos + 3);
        size_t end_pos = rest.find_first_of("/?#");
        auth = (end_pos == std::string_view::npos) ? rest : rest.substr(0, end_pos);
    } else {
        if (sv.find_first_of("/?#") != std::string_view::npos) {
            return "";
        }
        auth = sv;
    }
    if (auth.empty() || has_invalid_authority_chars(auth)) {
        return "";
    }
    return ascii_tolower(auth);
}

std::string extract_scheme_and_authority(std::string_view url_or_origin) {
    std::string_view sv = trim_ows(url_or_origin);
    if (sv.empty() || case_insensitive_equal(sv, "null")) {
        return "";
    }
    size_t scheme_pos = sv.find("://");
    if (scheme_pos == std::string_view::npos || scheme_pos == 0) {
        return "";
    }
    std::string scheme = ascii_tolower(sv.substr(0, scheme_pos));
    if (scheme != "http" && scheme != "https") {
        return "";
    }
    std::string_view rest = sv.substr(scheme_pos + 3);
    size_t end_pos = rest.find_first_of("/?#");
    std::string_view auth = (end_pos == std::string_view::npos) ? rest : rest.substr(0, end_pos);
    if (auth.empty() || has_invalid_authority_chars(auth)) {
        return "";
    }
    std::string auth_lower = ascii_tolower(auth);
    if (scheme == "http" && auth_lower.ends_with(":80")) {
        auth_lower.resize(auth_lower.size() - 3);
    } else if (scheme == "https" && auth_lower.ends_with(":443")) {
        auth_lower.resize(auth_lower.size() - 4);
    }
    return scheme + "://" + auth_lower;
}

} // namespace


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
    if (config.mode != CsrfMode::OriginOnly) {
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
        int origin_count = 0;
        std::string origin_hdr;
        int referer_count = 0;
        std::string referer_hdr;

        protocol::HttpHeaderCursor cursor(&ctx.raw_req());
        std::string hname, hval;
        while (cursor.next(hname, hval)) {
            if (case_insensitive_equal(hname, cfg.header_name)) {
                csrf_header_count++;
                if (csrf_header_count == 1) {
                    submitted_header_token = hval;
                }
            } else if (case_insensitive_equal(hname, "Origin")) {
                origin_count++;
                if (origin_count == 1) {
                    origin_hdr = hval;
                }
            } else if (case_insensitive_equal(hname, "Referer")) {
                referer_count++;
                if (referer_count == 1) {
                    referer_hdr = hval;
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

        auto handle_forbidden = [&](std::string_view reason, std::string_view origin = {}) {
            const std::string_view msg = (cfg.mode == CsrfMode::OriginOnly)
                ? "CSRF origin check failed"
                : "Invalid or missing CSRF token";
            auto err = HttpError::forbidden(std::string(msg), "CSRF_FAILED");
            err.with_diagnostic("csrf", std::string(reason));
            if (!origin.empty()) {
                err.with_internal_detail("origin", std::string(origin));
            }
            reply_error(ctx, err);
        };

        std::string_view fail_reason;
        std::string fail_origin;

        bool is_safe = false;
        for (const auto &m : cfg.safe_methods) {
            if (ctx.method_string() == m) {
                is_safe = true;
                break;
            }
        }

        if (is_safe) {
            if (cfg.mode == CsrfMode::OriginOnly) {
                co_await next();
                co_return;
            }

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

        auto verify_origin = [&]() -> bool {
            if (origin_count > 1) {
                fail_reason = "origin_duplicate";
                return false;
            }
            std::string_view candidate_hdr;
            bool used_referer = false;
            if (origin_count == 1 && !trim_ows(origin_hdr).empty()) {
                candidate_hdr = origin_hdr;
            } else if (origin_count == 0 && cfg.allow_referer_fallback) {
                if (referer_count > 1) {
                    fail_reason = "origin_duplicate";
                    return false;
                }
                if (referer_count == 1 && !trim_ows(referer_hdr).empty()) {
                    candidate_hdr = referer_hdr;
                    used_referer = true;
                } else {
                    fail_reason = "origin_missing";
                    return false;
                }
            } else {
                fail_reason = "origin_missing";
                return false;
            }

            std::string cand_norm = extract_scheme_and_authority(candidate_hdr);
            if (cand_norm.empty()) {
                fail_reason = "origin_malformed";
                return false;
            }

            std::vector<std::string> trusted_list;
            trusted_list.reserve(cfg.trusted_origins.size() + 1);
            for (const auto &t : cfg.trusted_origins) {
                if (!t.empty()) {
                    trusted_list.push_back(t);
                }
            }
            if (cfg.trusted_origin_resolver) {
                std::string dyn = cfg.trusted_origin_resolver(ctx);
                if (!dyn.empty()) {
                    trusted_list.push_back(std::move(dyn));
                }
            }

            if (!trusted_list.empty()) {
                for (const auto &t : trusted_list) {
                    std::string t_norm = extract_scheme_and_authority(t);
                    if (!t_norm.empty() && cand_norm == t_norm) {
                        return true;
                    }
                }
                fail_reason = used_referer ? "referer_untrusted" : "origin_untrusted";
                fail_origin = std::move(cand_norm);
                return false;
            }

            std::string host_auth = extract_authority_only(ctx.header_view("Host"));
            std::string cand_auth = extract_authority_only(cand_norm);
            if (host_auth.empty() || cand_auth != host_auth) {
                fail_reason = used_referer ? "referer_untrusted" : "origin_untrusted";
                fail_origin = std::move(cand_norm);
                return false;
            }
            return true;
        };

        auto verify_double_submit_token = [&]() -> bool {
            if (cookie_tokens.empty()) {
                fail_reason = "cookie_missing";
                return false;
            }
            if (cookie_tokens.size() != 1 || !verify_csrf_token(cookie_tokens[0], cfg.secret)) {
                fail_reason = "cookie_invalid";
                return false;
            }
            const std::string &cookie_token = cookie_tokens[0];

            if (csrf_header_count > 1) {
                fail_reason = "header_form_conflict";
                return false;
            }

            std::string form_token;
            bool has_form_token = false;
            if (cfg.allow_form_field) {
                std::string_view ct = ctx.content_type();
                if (ct.starts_with("application/x-www-form-urlencoded") || ct.starts_with("multipart/form-data")) {
                    if (has_form(ctx, cfg.form_field_name)) {
                        form_token = form(ctx, cfg.form_field_name);
                        has_form_token = true;
                    }
                }
            }

            std::string submitted_token;
            if (csrf_header_count == 1 && has_form_token) {
                if (submitted_header_token != form_token) {
                    fail_reason = "header_form_conflict";
                    return false;
                }
                submitted_token = submitted_header_token;
            } else if (csrf_header_count == 1) {
                submitted_token = submitted_header_token;
            } else if (has_form_token) {
                submitted_token = form_token;
            } else {
                fail_reason = "token_missing";
                return false;
            }

            if (submitted_token.empty() ||
                !verify_csrf_token(submitted_token, cfg.secret) ||
                !CryptoUtil::constant_time_equals(submitted_token, cookie_token)) {
                fail_reason = "token_mismatch";
                return false;
            }
            return true;
        };

        bool has_origin_policy = !cfg.trusted_origins.empty() || static_cast<bool>(cfg.trusted_origin_resolver);

        if (cfg.mode == CsrfMode::OriginOnly) {
            if (!verify_origin()) {
                handle_forbidden(fail_reason, fail_origin);
                co_return;
            }
            co_await next();
            co_return;
        }

        if (cfg.mode == CsrfMode::OriginOrToken) {
            if (verify_origin() || verify_double_submit_token()) {
                co_await next();
                co_return;
            }
            handle_forbidden(fail_reason, fail_origin);
            co_return;
        }

        // CsrfMode::TokenRequired
        if (has_origin_policy && !verify_origin()) {
            handle_forbidden(fail_reason, fail_origin);
            co_return;
        }
        if (!verify_double_submit_token()) {
            handle_forbidden(fail_reason, fail_origin);
            co_return;
        }

        co_await next();
    };
}

} // namespace kale