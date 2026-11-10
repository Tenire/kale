#ifndef KALE_HTTP_UTIL_H
#define KALE_HTTP_UTIL_H

#include <string>
#include <string_view>
#include <map>

namespace kale {

struct CaseInsensitiveLess {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
};

inline bool case_insensitive_equal(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char c1 = static_cast<unsigned char>(lhs[i]);
        const unsigned char c2 = static_cast<unsigned char>(rhs[i]);
        const unsigned char lower1 = (c1 >= 'A' && c1 <= 'Z') ? static_cast<unsigned char>(c1 + ('a' - 'A')) : c1;
        const unsigned char lower2 = (c2 >= 'A' && c2 <= 'Z') ? static_cast<unsigned char>(c2 + ('a' - 'A')) : c2;
        if (lower1 != lower2) {
            return false;
        }
    }
    return true;
}


bool is_valid_header_name(std::string_view name) noexcept;

bool is_valid_header_value(std::string_view value) noexcept;

bool is_valid_status_code(int status) noexcept;

bool is_valid_reason_phrase(std::string_view phrase) noexcept;

// Form query decoding ('+' decodes to space).
std::string url_decode(std::string_view str);

// Path percent-decoding (preserves '+').
std::string path_percent_decode(std::string_view str);

class PathUtil {
public:
    PathUtil() = delete;
    static bool needs_normalization(std::string_view path, bool preserve_trailing_slash = false) noexcept;
    static std::string clean_path(std::string_view path);
};

std::map<std::string, std::string, std::less<>> split_query(std::string_view query_str);

} // namespace kale

#endif // KALE_HTTP_UTIL_H
