#include "kale/core/http_util.h"

#include <algorithm>
#include <utility>
#include <vector>
namespace kale {

bool CaseInsensitiveLess::operator()(std::string_view lhs, std::string_view rhs) const noexcept {
    const size_t len = std::min(lhs.size(), rhs.size());
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c1 = static_cast<unsigned char>(lhs[i]);
        const unsigned char c2 = static_cast<unsigned char>(rhs[i]);
        const unsigned char lower1 = (c1 >= 'A' && c1 <= 'Z') ? static_cast<unsigned char>(c1 + ('a' - 'A')) : c1;
        const unsigned char lower2 = (c2 >= 'A' && c2 <= 'Z') ? static_cast<unsigned char>(c2 + ('a' - 'A')) : c2;
        if (lower1 != lower2) {
            return lower1 < lower2;
        }
    }
    return lhs.size() < rhs.size();
}

bool is_valid_header_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 32 || uc >= 127) {
            return false;
        }
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

bool is_valid_header_value(std::string_view value) noexcept {
    for (char c : value) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return false;
        }
    }
    return true;
}


bool is_valid_status_code(int status) noexcept {
    return status >= 100 && status <= 999;
}

bool is_valid_reason_phrase(std::string_view phrase) noexcept {
    for (char c : phrase) {
        const unsigned char uc = static_cast<unsigned char>(c);
        const bool valid = (uc == '\t' || uc == ' ' || (uc >= 0x21 && uc <= 0x7e) || uc >= 0x80);
        if (!valid) {
            return false;
        }
    }
    return true;
}

std::string url_decode(std::string_view str) {
    std::string decoded;
    decoded.reserve(str.size());

    auto hex_to_val = [](char ch) noexcept -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int h1 = hex_to_val(str[i + 1]);
            int h2 = hex_to_val(str[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                decoded.push_back(static_cast<char>((h1 << 4) | h2));
                i += 2;
                continue;
            }
        } else if (str[i] == '+') {
            decoded.push_back(' ');
            continue;
        }
        decoded.push_back(str[i]);
    }
    return decoded;
}

std::string path_percent_decode(std::string_view str) {
    std::string decoded;
    decoded.reserve(str.size());

    auto hex_to_val = [](char ch) noexcept -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int h1 = hex_to_val(str[i + 1]);
            int h2 = hex_to_val(str[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                decoded.push_back(static_cast<char>((h1 << 4) | h2));
                i += 2;
                continue;
            }
        }
        decoded.push_back(str[i]);
    }

    return decoded;
}
bool PathUtil::needs_normalization(std::string_view path, bool preserve_trailing_slash) noexcept {
    if (path.empty()) {
        return true;
    }

    const size_t n = path.size();
    const bool rooted = (path.front() == '/');

    if (rooted) {
        if (n == 1) {
            return false;
        }

        if (path.back() == '/') {
            if (!preserve_trailing_slash) {
                return true;
            }
            if (n >= 2 && path[n - 2] == '/') {
                return true;
            }
        }

        size_t i = 0;
        while (i < n) {
            if (path[i] == '/') {
                if (i + 1 < n && path[i + 1] == '/') {
                    return true;
                }
                ++i;
                continue;
            }

            const size_t start = i;
            while (i < n && path[i] != '/') {
                ++i;
            }
            const std::string_view seg = path.substr(start, i - start);
            if (seg == "." || seg == "..") {
                return true;
            }
        }
        return false;
    } else {
        if (path == ".") {
            return false;
        }

        if (path.back() == '/') {
            if (!preserve_trailing_slash) {
                return true;
            }
            if (n >= 2 && path[n - 2] == '/') {
                return true;
            }
        }

        bool has_non_dotdot = false;
        size_t i = 0;
        while (i < n) {
            if (path[i] == '/') {
                if (i + 1 < n && path[i + 1] == '/') {
                    return true;
                }
                ++i;
                continue;
            }

            const size_t start = i;
            while (i < n && path[i] != '/') {
                ++i;
            }
            const std::string_view seg = path.substr(start, i - start);
            if (seg == ".") {
                return true;
            } else if (seg == "..") {
                if (has_non_dotdot) {
                    return true;
                }
            } else {
                has_non_dotdot = true;
            }
        }
        return false;
    }
}

std::string PathUtil::clean_path(std::string_view path) {
    if (!needs_normalization(path)) {
        return std::string(path);
    }

    if (path.empty()) {
        return ".";
    }

    const bool rooted = (path.front() == '/');
    const size_t n = path.size();

    if (rooted && path.find_first_not_of('/') == std::string_view::npos) {
        return "/";
    }

    std::vector<std::string_view> segments;
    size_t i = 0;
    while (i < n) {
        while (i < n && path[i] == '/') {
            ++i;
        }
        if (i >= n) {
            break;
        }
        size_t start = i;
        while (i < n && path[i] != '/') {
            ++i;
        }
        std::string_view seg = path.substr(start, i - start);
        if (seg == ".") {
            continue;
        } else if (seg == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (!rooted) {
                segments.push_back(seg);
            }
        } else {
            segments.push_back(seg);
        }
    }

    if (rooted) {
        if (segments.empty()) {
            return "/";
        }
        std::string res;
        for (const auto &seg : segments) {
            res.push_back('/');
            res.append(seg);
        }
        return res;
    } else {
        if (segments.empty()) {
            return ".";
        }
        std::string res;
        bool first = true;
        for (const auto &seg : segments) {
            if (!first) {
                res.push_back('/');
            }
            first = false;
            res.append(seg);
        }
        return res;
    }
}


std::map<std::string, std::string, std::less<>> split_query(std::string_view query_str) {
    std::map<std::string, std::string, std::less<>> params;
    while (!query_str.empty()) {
        size_t amp = query_str.find('&');
        std::string_view field = (amp != std::string_view::npos) ? query_str.substr(0, amp) : query_str;
        if (!field.empty()) {
            size_t eq = field.find('=');
            if (eq != std::string_view::npos) {
                std::string k = url_decode(field.substr(0, eq));
                if (!k.empty()) {
                    std::string v = url_decode(field.substr(eq + 1));
                    params.emplace(std::move(k), std::move(v));
                }
            } else {
                std::string k = url_decode(field);
                if (!k.empty()) {
                    params.emplace(std::move(k), "");
                }
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query_str.remove_prefix(amp + 1);
    }
    return params;
}

} // namespace kale
