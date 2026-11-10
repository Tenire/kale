#include "kale/security/signed_cookie.h"
#include "kale/core/context.h"
#include "security/crypto_util.h"

namespace kale {

std::string sign_cookie_value(std::string_view val, std::string_view secret) {
    std::string sig = CryptoUtil::hmac_sha256_base64url(val, secret);
    std::string res;
    res.reserve(val.size() + 1 + sig.size());
    res.append(val);
    res.push_back('.');
    res.append(sig);
    return res;
}

std::optional<std::string> unsign_cookie_value(std::string_view signed_val, std::string_view secret) {
    size_t last_dot = signed_val.rfind('.');
    if (last_dot == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view raw_val = signed_val.substr(0, last_dot);
    std::string_view actual_sig = signed_val.substr(last_dot + 1);

    std::string expected_sig = CryptoUtil::hmac_sha256_base64url(raw_val, secret);

    if (!CryptoUtil::constant_time_equals(actual_sig, expected_sig)) {
        return std::nullopt;
    }

    return std::string(raw_val);
}

Context &set_signed_cookie(Context &ctx, Cookie cookie, std::string_view secret) {
    std::string signed_val = sign_cookie_value(cookie.value(), secret);
    cookie.set_value(std::move(signed_val));
    return ctx.set_cookie(std::move(cookie));
}

Context &set_signed_cookie(Context &ctx, std::string name, std::string value, std::string_view secret) {
    std::string signed_val = sign_cookie_value(value, secret);
    return ctx.set_cookie(Cookie(std::move(name), std::move(signed_val)));
}

std::optional<std::string> signed_cookie(const Context &ctx, std::string_view name, std::string_view secret) {
    std::string_view val = ctx.cookie(name);
    if (val.empty()) {
        return std::nullopt;
    }
    return unsign_cookie_value(val, secret);
}

} // namespace kale
