/**
 * @file validator.h
 * @brief Typed constraint descriptors and validation rules for DTO fields.
 */
#ifndef KALE_VALIDATOR_H
#define KALE_VALIDATOR_H

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <kale/core/http_error.h>

namespace kale {

struct FieldError {
    std::string field;
    std::string reason;
    std::string value;
};

class ValidationResult {
public:
    ValidationResult() = default;

    bool is_valid() const noexcept { return errors_.empty(); }
    explicit operator bool() const noexcept { return is_valid(); }

    void add_error(std::string field, std::string reason, std::string value = "") {
        errors_.push_back(FieldError{std::move(field), std::move(reason), std::move(value)});
    }

    void merge(ValidationResult other) {
        errors_.insert(errors_.end(),
                       std::make_move_iterator(other.errors_.begin()),
                       std::make_move_iterator(other.errors_.end()));
    }

    const std::vector<FieldError> &errors() const noexcept { return errors_; }

    void throw_if_invalid(std::string message = "Validation Failed", std::string error_code = "VALIDATION_FAILED") const;
private:
    std::vector<FieldError> errors_;
};
class ValidationError : public HttpError {
public:
    explicit ValidationError(const ValidationResult &result,
                             std::string message = "Validation Failed",
                             std::string error_code = "VALIDATION_FAILED")
        : HttpError(static_cast<int>(HttpStatus::UnprocessableEntity), std::move(message), std::move(error_code), true) {
        for (const auto &err : result.errors()) {
            with_detail(err.field, err.reason);
        }
    }
};

inline void ValidationResult::throw_if_invalid(std::string message, std::string error_code) const {
    if (!is_valid()) {
        throw ValidationError(*this, std::move(message), std::move(error_code));
    }
}

namespace rules {


template <typename A, typename B>
constexpr bool compare_lt(const A &a, const B &b) noexcept {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        return std::cmp_less(a, b);
    } else {
        return a < b;
    }
}

template <typename A, typename B>
constexpr bool compare_lte(const A &a, const B &b) noexcept {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        return std::cmp_less_equal(a, b);
    } else {
        return a <= b;
    }
}

template <typename A, typename B>
constexpr bool compare_gt(const A &a, const B &b) noexcept {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        return std::cmp_greater(a, b);
    } else {
        return a > b;
    }
}

template <typename A, typename B>
constexpr bool compare_gte(const A &a, const B &b) noexcept {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        return std::cmp_greater_equal(a, b);
    } else {
        return a >= b;
    }
}

template <typename A, typename B>
constexpr bool compare_eq(const A &a, const B &b) noexcept {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        return std::cmp_equal(a, b);
    } else {
        return a == b;
    }
}


struct RequiredRule {
    static constexpr std::string_view name = "required";

    template <typename T>
    constexpr bool check(const T &) const noexcept {
        return true;
    }

    template <typename T>
    constexpr std::optional<std::string> operator()(const T &) const noexcept {
        return std::nullopt;
    }

    std::string expected() const { return "present"; }
    std::string reason() const { return "required field is missing or null"; }
};

inline constexpr RequiredRule required() noexcept {
    return RequiredRule{};
}

struct MinLenRule {
    static constexpr std::string_view name = "min_len";
    size_t min_size{0};

    constexpr explicit MinLenRule(size_t s) noexcept : min_size(s) {}

    constexpr bool check(std::string_view val) const noexcept {
        return val.size() >= min_size;
    }

    template <typename T>
        requires (!std::convertible_to<const T &, std::string_view>) && requires(const T &t) { t.size(); }
    constexpr bool check(const T &val) const noexcept {
        return val.size() >= min_size;
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    template <typename T>
        requires (!std::convertible_to<const T &, std::string_view>) && requires(const T &t) { t.size(); }
    std::optional<std::string> operator()(const T &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        return "length >= " + std::to_string(min_size);
    }

    std::string reason() const {
        return "length must be >= " + std::to_string(min_size);
    }
};

inline constexpr MinLenRule min_len(size_t min_size) noexcept {
    return MinLenRule{min_size};
}

struct MaxLenRule {
    static constexpr std::string_view name = "max_len";
    size_t max_size{0};

    constexpr explicit MaxLenRule(size_t s) noexcept : max_size(s) {}

    constexpr bool check(std::string_view val) const noexcept {
        return val.size() <= max_size;
    }

    template <typename T>
        requires (!std::convertible_to<const T &, std::string_view>) && requires(const T &t) { t.size(); }
    constexpr bool check(const T &val) const noexcept {
        return val.size() <= max_size;
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    template <typename T>
        requires (!std::convertible_to<const T &, std::string_view>) && requires(const T &t) { t.size(); }
    std::optional<std::string> operator()(const T &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        return "length <= " + std::to_string(max_size);
    }

    std::string reason() const {
        return "length must be <= " + std::to_string(max_size);
    }
};

inline constexpr MaxLenRule max_len(size_t max_size) noexcept {
    return MaxLenRule{max_size};
}

/**
 * @brief Length range rule for strings and vectors.
 * @throws std::invalid_argument If min_size exceeds max_size.
 */
struct LenRangeRule {
    static constexpr std::string_view name = "len_range";
    size_t min_size{0};
    size_t max_size{0};

    constexpr LenRangeRule(size_t mn, size_t mx) : min_size(mn), max_size(mx) {
        if (mn > mx) {
            throw std::invalid_argument("kale::rules::len_range: min_size exceeds max_size");
        }
    }

    constexpr bool check(std::string_view val) const noexcept {
        return val.size() >= min_size && val.size() <= max_size;
    }

    template <typename T>
        requires (!std::convertible_to<const T &, std::string_view>) && requires(const T &t) { t.size(); }
    constexpr bool check(const T &val) const noexcept {
        return val.size() >= min_size && val.size() <= max_size;
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    template <typename T>
        requires (!std::convertible_to<const T &, std::string_view>) && requires(const T &t) { t.size(); }
    std::optional<std::string> operator()(const T &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        return "length between " + std::to_string(min_size) + " and " + std::to_string(max_size);
    }

    std::string reason() const {
        return "length must be between " + std::to_string(min_size) + " and " + std::to_string(max_size);
    }
};

inline constexpr LenRangeRule len_range(size_t min_size, size_t max_size) {
    return LenRangeRule{min_size, max_size};
}

template <typename T>
struct GtRule {
    static constexpr std::string_view name = "gt";
    T boundary;

    constexpr explicit GtRule(T b) : boundary(b) {
        if constexpr (std::floating_point<T>) {
            if (!std::isfinite(b)) {
                throw std::invalid_argument("kale::rules::gt: boundary must be finite");
            }
        }
    }

    template <typename Val>
    constexpr bool check(const Val &val) const noexcept {
        return compare_gt(val, boundary);
    }

    template <typename Val>
    std::optional<std::string> operator()(const Val &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        std::ostringstream oss;
        oss << "> " << boundary;
        return oss.str();
    }

    std::string reason() const {
        std::ostringstream oss;
        oss << "value must be > " << boundary;
        return oss.str();
    }
};

template <typename T>
    requires std::totally_ordered<T>
inline constexpr auto gt(T boundary) {
    return GtRule<T>{boundary};
}

template <typename T>
struct GteRule {
    static constexpr std::string_view name = "gte";
    T boundary;

    constexpr explicit GteRule(T b) : boundary(b) {
        if constexpr (std::floating_point<T>) {
            if (!std::isfinite(b)) {
                throw std::invalid_argument("kale::rules::gte: boundary must be finite");
            }
        }
    }

    template <typename Val>
    constexpr bool check(const Val &val) const noexcept {
        return compare_gte(val, boundary);
    }

    template <typename Val>
    std::optional<std::string> operator()(const Val &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        std::ostringstream oss;
        oss << ">= " << boundary;
        return oss.str();
    }

    std::string reason() const {
        std::ostringstream oss;
        oss << "value must be >= " << boundary;
        return oss.str();
    }
};

template <typename T>
    requires std::totally_ordered<T>
inline constexpr auto gte(T boundary) {
    return GteRule<T>{boundary};
}

template <typename T>
struct LtRule {
    static constexpr std::string_view name = "lt";
    T boundary;

    constexpr explicit LtRule(T b) : boundary(b) {
        if constexpr (std::floating_point<T>) {
            if (!std::isfinite(b)) {
                throw std::invalid_argument("kale::rules::lt: boundary must be finite");
            }
        }
    }

    template <typename Val>
    constexpr bool check(const Val &val) const noexcept {
        return compare_lt(val, boundary);
    }

    template <typename Val>
    std::optional<std::string> operator()(const Val &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        std::ostringstream oss;
        oss << "< " << boundary;
        return oss.str();
    }

    std::string reason() const {
        std::ostringstream oss;
        oss << "value must be < " << boundary;
        return oss.str();
    }
};

template <typename T>
    requires std::totally_ordered<T>
inline constexpr auto lt(T boundary) {
    return LtRule<T>{boundary};
}

template <typename T>
struct LteRule {
    static constexpr std::string_view name = "lte";
    T boundary;

    constexpr explicit LteRule(T b) : boundary(b) {
        if constexpr (std::floating_point<T>) {
            if (!std::isfinite(b)) {
                throw std::invalid_argument("kale::rules::lte: boundary must be finite");
            }
        }
    }

    template <typename Val>
    constexpr bool check(const Val &val) const noexcept {
        return compare_lte(val, boundary);
    }

    template <typename Val>
    std::optional<std::string> operator()(const Val &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        std::ostringstream oss;
        oss << "<= " << boundary;
        return oss.str();
    }

    std::string reason() const {
        std::ostringstream oss;
        oss << "value must be <= " << boundary;
        return oss.str();
    }
};

template <typename T>
    requires std::totally_ordered<T>
inline constexpr auto lte(T boundary) {
    return LteRule<T>{boundary};
}

/**
 * @brief Closed range rule [min_v, max_v].
 * @throws std::invalid_argument If min_v > max_v or bounds are non-finite.
 */
template <typename Low, typename High = Low>
struct RangeRule {
    static constexpr std::string_view name = "range";
    Low min_v;
    High max_v;

    constexpr RangeRule(Low mn, High mx) : min_v(mn), max_v(mx) {
        if constexpr (std::floating_point<Low>) {
            if (!std::isfinite(mn)) throw std::invalid_argument("kale::rules::range: min_v must be finite");
        }
        if constexpr (std::floating_point<High>) {
            if (!std::isfinite(mx)) throw std::invalid_argument("kale::rules::range: max_v must be finite");
        }
        if (compare_gt(mn, mx)) {
            throw std::invalid_argument("kale::rules::range: min_v cannot exceed max_v");
        }
    }

    template <typename Val>
    constexpr bool check(const Val &val) const noexcept {
        return compare_gte(val, min_v) && compare_lte(val, max_v);
    }

    template <typename Val>
    std::optional<std::string> operator()(const Val &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        std::ostringstream oss;
        oss << "between " << min_v << " and " << max_v;
        return oss.str();
    }

    std::string reason() const {
        std::ostringstream oss;
        oss << "value must be between " << min_v << " and " << max_v;
        return oss.str();
    }
};

template <typename Low, typename High>
inline constexpr auto range(Low min_v, High max_v) {
    return RangeRule<Low, High>{min_v, max_v};
}

/**
 * @brief Regular expression pattern rule.
 * Compiles std::regex upon construction.
 * @throws std::invalid_argument If regex syntax is invalid.
 */
class PatternRule {
public:
    static constexpr std::string_view name = "pattern";

    explicit PatternRule(std::string pattern_str)
        : pattern_(std::move(pattern_str)),
          regex_(compile_pattern(pattern_)) {}

    explicit PatternRule(std::string_view pattern_str)
        : pattern_(pattern_str),
          regex_(compile_pattern(pattern_)) {}

    explicit PatternRule(const char *pattern_str)
        : pattern_(pattern_str ? pattern_str : ""),
          regex_(compile_pattern(pattern_)) {}

    PatternRule(const PatternRule &other)
        : pattern_(other.pattern_),
          regex_(compile_pattern(pattern_)) {}

    PatternRule(PatternRule &&other) noexcept = default;
    PatternRule &operator=(const PatternRule &other) {
        if (this != &other) {
            pattern_ = other.pattern_;
            regex_ = compile_pattern(pattern_);
        }
        return *this;
    }
    PatternRule &operator=(PatternRule &&other) noexcept = default;

    const std::string &pattern() const noexcept { return pattern_; }
    const std::regex &regex() const noexcept { return regex_; }

    bool check(std::string_view val) const {
        return std::regex_match(val.begin(), val.end(), regex_);
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        return "pattern matching /" + pattern_ + "/";
    }

    std::string reason() const {
        return "value does not match pattern /" + pattern_ + "/";
    }

private:
    static std::regex compile_pattern(const std::string &pat) {
        try {
            return std::regex(pat, std::regex::ECMAScript);
        } catch (const std::regex_error &e) {
            throw std::invalid_argument("kale::rules::pattern: invalid regular expression '" + pat + "': " + e.what());
        }
    }

    std::string pattern_;
    std::regex regex_;
};

inline auto pattern(std::string_view regex_str) {
    return PatternRule{regex_str};
}

template <typename T>
struct OneOfRule {
    static constexpr std::string_view name = "one_of";
    std::vector<T> choices;

    constexpr explicit OneOfRule(std::vector<T> c) : choices(std::move(c)) {}

    template <typename Val>
    bool check(const Val &val) const {
        for (const auto &item : choices) {
            if constexpr (std::is_integral_v<Val> && std::is_integral_v<T>) {
                if (std::cmp_equal(val, item)) return true;
            } else {
                if (val == item) return true;
            }
        }
        return false;
    }

    template <typename Val>
    std::optional<std::string> operator()(const Val &val) const {
        if (!check(val)) {
            return reason();
        }
        return std::nullopt;
    }

    std::string expected() const {
        std::ostringstream oss;
        oss << "one of [";
        for (size_t i = 0; i < choices.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << choices[i];
        }
        oss << "]";
        return oss.str();
    }

    std::string reason() const {
        return "value is not in allowed choices";
    }
};

template <typename T>
inline auto one_of(std::vector<T> choices) {
    return OneOfRule<T>{std::move(choices)};
}

template <typename T>
inline auto one_of(std::initializer_list<T> list) {
    return OneOfRule<T>{std::vector<T>(list)};
}

template <typename T, std::size_t N>
inline auto one_of(const std::array<T, N> &arr) {
    return OneOfRule<T>{std::vector<T>(arr.begin(), arr.end())};
}

template <typename T, std::size_t N>
inline auto one_of(const T (&arr)[N]) {
    return OneOfRule<T>{std::vector<T>(arr, arr + N)};
}


inline bool is_ascii_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline bool is_ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool is_ascii_alnum(char c) noexcept {
    return is_ascii_alpha(c) || is_ascii_digit(c);
}

inline bool is_hex_digit(char c) noexcept {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool check_email_format(std::string_view val) noexcept {
    if (val.size() < 3 || val.size() > 254) return false;
    size_t at_pos = val.find('@');
    if (at_pos == std::string_view::npos || at_pos == 0 || at_pos > 64) return false;
    std::string_view local = val.substr(0, at_pos);
    std::string_view domain = val.substr(at_pos + 1);
    if (domain.empty() || domain.size() > 253) return false;
    if (local.front() == '.' || local.back() == '.') return false;
    bool prev_dot = false;
    for (char c : local) {
        bool ok = is_ascii_alnum(c) || c == '_' || c == '.' || c == '+' || c == '-';
        if (!ok) return false;
        if (c == '.') {
            if (prev_dot) return false;
            prev_dot = true;
        } else {
            prev_dot = false;
        }
    }
    size_t label_start = 0;
    size_t label_count = 0;
    bool last_label_all_numeric = true;
    size_t last_label_len = 0;
    while (label_start <= domain.size()) {
        size_t dot = domain.find('.', label_start);
        std::string_view label = (dot == std::string_view::npos)
                                 ? domain.substr(label_start)
                                 : domain.substr(label_start, dot - label_start);
        if (label.empty() || label.size() > 63) return false;
        if (label.front() == '-' || label.back() == '-') return false;
        bool all_num = true;
        for (char c : label) {
            if (!is_ascii_alnum(c) && c != '-') return false;
            if (!is_ascii_digit(c)) all_num = false;
        }
        ++label_count;
        last_label_all_numeric = all_num;
        last_label_len = label.size();
        if (dot == std::string_view::npos) break;
        label_start = dot + 1;
    }
    if (label_count < 2 || last_label_len < 2 || last_label_all_numeric) return false;
    return true;
}

inline bool check_uuid_v7_format(std::string_view val) noexcept {
    if (val.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (val[i] != '-') return false;
        } else {
            if (!is_hex_digit(val[i])) return false;
        }
    }
    if (val[14] != '7') return false;
    char var = val[19];
    if (var != '8' && var != '9' && var != 'a' && var != 'b' && var != 'A' && var != 'B') return false;
    return true;
}

struct IsEmailRule {
    static constexpr std::string_view name = "is_email";

    bool check(std::string_view val) const noexcept {
        return check_email_format(val);
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "valid email address"; }
    std::string reason() const { return "invalid email format"; }
};

inline auto is_email() { return IsEmailRule{}; }

struct IsUuidV7Rule {
    static constexpr std::string_view name = "is_uuid_v7";

    bool check(std::string_view val) const noexcept {
        return check_uuid_v7_format(val);
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "valid UUIDv7"; }
    std::string reason() const { return "invalid UUIDv7 format"; }
};

inline auto is_uuid_v7() { return IsUuidV7Rule{}; }
inline auto is_uuid() { return IsUuidV7Rule{}; }

struct IsNumericRule {
    static constexpr std::string_view name = "is_numeric";

    bool check(std::string_view val) const noexcept {
        if (val.empty()) return false;
        for (char c : val) {
            if (!is_ascii_digit(c)) return false;
        }
        return true;
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "numeric string"; }
    std::string reason() const { return "must contain only digits"; }
};

inline auto is_numeric() { return IsNumericRule{}; }

struct IsAlphanumericRule {
    static constexpr std::string_view name = "is_alphanumeric";

    bool check(std::string_view val) const noexcept {
        if (val.empty()) return false;
        for (char c : val) {
            if (!is_ascii_alnum(c)) return false;
        }
        return true;
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "alphanumeric string"; }
    std::string reason() const { return "must contain only letters and digits"; }
};

inline auto is_alphanumeric() { return IsAlphanumericRule{}; }

struct IsHexRule {
    static constexpr std::string_view name = "is_hex";

    bool check(std::string_view val) const noexcept {
        if (val.empty()) return false;
        for (char c : val) {
            if (!is_hex_digit(c)) return false;
        }
        return true;
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "hexadecimal string"; }
    std::string reason() const { return "must contain only hexadecimal characters"; }
};

inline auto is_hex() { return IsHexRule{}; }

struct StartsWithRule {
    static constexpr std::string_view name = "starts_with";
    std::string prefix;

    explicit StartsWithRule(std::string p) : prefix(std::move(p)) {}

    bool check(std::string_view val) const noexcept {
        return val.starts_with(prefix);
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "prefix '" + prefix + "'"; }
    std::string reason() const { return "must start with '" + prefix + "'"; }
};

inline auto starts_with(std::string prefix) { return StartsWithRule{std::move(prefix)}; }

struct EndsWithRule {
    static constexpr std::string_view name = "ends_with";
    std::string suffix;

    explicit EndsWithRule(std::string s) : suffix(std::move(s)) {}

    bool check(std::string_view val) const noexcept {
        return val.ends_with(suffix);
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "suffix '" + suffix + "'"; }
    std::string reason() const { return "must end with '" + suffix + "'"; }
};

inline auto ends_with(std::string suffix) { return EndsWithRule{std::move(suffix)}; }

struct ContainsRule {
    static constexpr std::string_view name = "contains";
    std::string needle;

    explicit ContainsRule(std::string n) : needle(std::move(n)) {}

    bool check(std::string_view val) const noexcept {
        return val.find(needle) != std::string_view::npos;
    }

    std::optional<std::string> operator()(std::string_view val) const {
        if (!check(val)) return reason();
        return std::nullopt;
    }

    std::string expected() const { return "containing '" + needle + "'"; }
    std::string reason() const { return "must contain '" + needle + "'"; }
};

inline auto contains(std::string needle) { return ContainsRule{std::move(needle)}; }

template <typename F>
struct CustomRule {
    static constexpr std::string_view name = "custom";
    F fn;

    constexpr explicit CustomRule(F &&f) : fn(std::forward<F>(f)) {}

    template <typename Val>
    bool check(const Val &val) const {
        using Ret = std::invoke_result_t<F, decltype(val)>;
        if constexpr (std::is_same_v<Ret, bool>) {
            return fn(val);
        } else if constexpr (std::is_same_v<Ret, std::optional<std::string>>) {
            return !fn(val).has_value();
        } else if constexpr (std::is_convertible_v<Ret, std::string>) {
            return std::string(fn(val)).empty();
        } else {
            static_assert(!sizeof(Ret), "custom rule function must return bool, std::string or std::optional<std::string>");
        }
    }

    template <typename Val>
    std::optional<std::string> operator()(const Val &val) const {
        using Ret = std::invoke_result_t<F, decltype(val)>;
        if constexpr (std::is_same_v<Ret, bool>) {
            if (!fn(val)) return reason();
            return std::nullopt;
        } else if constexpr (std::is_same_v<Ret, std::optional<std::string>>) {
            return fn(val);
        } else if constexpr (std::is_convertible_v<Ret, std::string>) {
            std::string err = fn(val);
            if (!err.empty()) return err;
            return std::nullopt;
        }
    }

    std::string expected() const { return "custom validation criteria"; }
    std::string reason() const { return "custom validation failed"; }
};

template <typename F>
inline auto custom(F &&fn) {
    return CustomRule<std::decay_t<F>>{std::forward<F>(fn)};
}

} // namespace rules

} // namespace kale

#endif // KALE_VALIDATOR_H