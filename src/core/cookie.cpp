#include "kale/core/cookie.h"
#include <stdexcept>
#include "kale/core/context.h"
#include "workflow/HttpMessage.h"

namespace kale {

static bool is_valid_cookie_token(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 32 || uc >= 127) return false;
        switch (c) {
            case '(': case ')': case '<': case '>': case '@':
            case ',': case ';': case ':': case '\\': case '"':
            case '/': case '[': case ']': case '?': case '=':
            case '{': case '}':
                return false;
            default:
                break;
        }
    }
    return true;
}

static bool is_valid_cookie_value(std::string_view s) noexcept {
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '\r' || c == '\n' || c == '\0' || c == ';' || uc < 32 || uc == 127) {
            return false;
        }
    }
    return true;
}

bool Cookie::is_valid() const noexcept {
    if (!is_valid_cookie_token(name_)) return false;
    if (!is_valid_cookie_value(value_)) return false;
    if (!path_.empty() && !is_valid_cookie_value(path_)) return false;
    if (!domain_.empty() && !is_valid_cookie_value(domain_)) return false;
    return true;
}

std::string Cookie::to_string() const {
    if (!is_valid()) {
        throw std::invalid_argument("Invalid cookie name, value, or attribute");
    }
    std::string res;
    res.reserve(64 + name_.size() + value_.size() + path_.size() + domain_.size());
    res.append(name_).append("=").append(value_);

    if (!path_.empty()) {
        res.append("; Path=").append(path_);
    }
    if (!domain_.empty()) {
        res.append("; Domain=").append(domain_);
    }
    if (max_age_ >= 0) {
        res.append("; Max-Age=").append(std::to_string(max_age_));
    }
    if (secure_) {
        res.append("; Secure");
    }
    if (http_only_) {
        res.append("; HttpOnly");
    }
    switch (same_site_) {
        case SameSite::Strict:
            res.append("; SameSite=Strict");
            break;
        case SameSite::Lax:
            res.append("; SameSite=Lax");
            break;
        case SameSite::None:
            res.append("; SameSite=None");
            break;
    }
    return res;
}

Context &Context::set_cookie(Cookie cookie) {
    if (!cookie.is_valid()) throw std::invalid_argument("Invalid response cookie");
    const std::string cookie_str = cookie.to_string();
    if (!cookie_str.empty()) {
        protocol::HttpMessageHeader h;
        h.name = "Set-Cookie";
        h.name_len = 10;
        h.value = cookie_str.data();
        h.value_len = cookie_str.size();
        raw_resp().add_header(&h);
    }
    return *this;
}

Context &Context::set_cookie(std::string name, std::string value) {
    return set_cookie(Cookie(std::move(name), std::move(value)));
}

} // namespace kale
