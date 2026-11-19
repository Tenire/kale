#include "kale/security/signed_cookie.h"
#include "kale/core/context.h"
#include "security/crypto_util.h"
#include <charconv>

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

std::string sign_cookie_value_with_ttl(
    std::string_view val,
    std::string_view secret,
    int64_t issued_at_epoch_sec) {
    if (issued_at_epoch_sec < 0) {
        issued_at_epoch_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::string ts = std::to_string(issued_at_epoch_sec);
    std::string payload;
    payload.reserve(val.size() + 1 + ts.size());
    payload.append(val);
    payload.push_back('.');
    payload.append(ts);

    std::string sig = CryptoUtil::hmac_sha256_base64url(payload, secret);
    std::string res;
    res.reserve(payload.size() + 1 + sig.size());
    res.append(payload);
    res.push_back('.');
    res.append(sig);
    return res;
}

std::optional<std::string> unsign_cookie_value_with_ttl(
    std::string_view signed_val,
    std::string_view secret,
    std::chrono::seconds max_age,
    int64_t now_epoch_sec) {
    if (max_age.count() < 0) {
        return std::nullopt;
    }
    size_t last_dot = signed_val.rfind('.');
    if (last_dot == std::string_view::npos || last_dot == 0) {
        return std::nullopt;
    }

    std::string_view payload_with_ts = signed_val.substr(0, last_dot);
    std::string_view actual_sig = signed_val.substr(last_dot + 1);

    std::string expected_sig = CryptoUtil::hmac_sha256_base64url(payload_with_ts, secret);
    if (!CryptoUtil::constant_time_equals(actual_sig, expected_sig)) {
        return std::nullopt;
    }

    size_t ts_dot = payload_with_ts.rfind('.');
    if (ts_dot == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view raw_val = payload_with_ts.substr(0, ts_dot);
    std::string_view ts_str = payload_with_ts.substr(ts_dot + 1);
    if (ts_str.empty()) {
        return std::nullopt;
    }
    for (char c : ts_str) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
    }

    int64_t issued_at = 0;
    auto [ptr, ec] = std::from_chars(ts_str.data(), ts_str.data() + ts_str.size(), issued_at);
    if (ec != std::errc{} || ptr != ts_str.data() + ts_str.size()) {
        return std::nullopt;
    }

    int64_t now = (now_epoch_sec >= 0)
        ? now_epoch_sec
        : std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();

    if (now < issued_at || (now - issued_at) > max_age.count()) {
        return std::nullopt;
    }

    return std::string(raw_val);
}

Context &set_signed_cookie_with_ttl(
    Context &ctx,
    Cookie cookie,
    std::string_view secret,
    std::chrono::seconds max_age) {
    std::string signed_val = sign_cookie_value_with_ttl(cookie.value(), secret);
    cookie.set_value(std::move(signed_val));
    cookie.set_max_age(static_cast<int>(max_age.count()));
    return ctx.set_cookie(std::move(cookie));
}

Context &set_signed_cookie_with_ttl(
    Context &ctx,
    std::string name,
    std::string value,
    std::string_view secret,
    std::chrono::seconds max_age) {
    Cookie cookie(std::move(name), std::move(value));
    return set_signed_cookie_with_ttl(ctx, std::move(cookie), secret, max_age);
}

std::optional<std::string> signed_cookie_with_ttl(
    const Context &ctx,
    std::string_view name,
    std::string_view secret,
    std::chrono::seconds max_age) {
    std::string_view val = ctx.cookie(name);
    if (val.empty()) {
        return std::nullopt;
    }
    return unsign_cookie_value_with_ttl(val, secret, max_age);
}

} // namespace kale
