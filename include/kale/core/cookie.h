#ifndef KALE_COOKIE_H
#define KALE_COOKIE_H

#include <string>

namespace kale {

class Cookie {
public:
    enum class SameSite {
        None,
        Lax,
        Strict
    };

    Cookie() = default;
    Cookie(std::string name, std::string value)
        : name_(std::move(name)), value_(std::move(value)) {}

    const std::string &name() const noexcept { return name_; }
    const std::string &value() const noexcept { return value_; }
    const std::string &path() const noexcept { return path_; }
    const std::string &domain() const noexcept { return domain_; }
    int max_age() const noexcept { return max_age_; }
    bool secure() const noexcept { return secure_; }
    bool http_only() const noexcept { return http_only_; }
    SameSite same_site() const noexcept { return same_site_; }

    Cookie &set_name(std::string name) { name_ = std::move(name); return *this; }
    Cookie &set_value(std::string value) { value_ = std::move(value); return *this; }
    Cookie &set_path(std::string path) { path_ = std::move(path); return *this; }
    Cookie &set_domain(std::string domain) { domain_ = std::move(domain); return *this; }
    Cookie &set_max_age(int seconds) noexcept { max_age_ = seconds; return *this; }
    Cookie &set_secure(bool secure) noexcept { secure_ = secure; return *this; }
    Cookie &set_http_only(bool http_only) noexcept { http_only_ = http_only; return *this; }
    Cookie &set_same_site(SameSite same_site) noexcept { same_site_ = same_site; return *this; }

    std::string to_string() const;
    bool is_valid() const noexcept;

private:
    std::string name_;
    std::string value_;
    std::string path_{"/"};
    std::string domain_;
    int max_age_{-1}; // -1 means session cookie
    bool secure_{false};
    bool http_only_{true};
    SameSite same_site_{SameSite::Lax};
};

} // namespace kale

#endif // KALE_COOKIE_H
