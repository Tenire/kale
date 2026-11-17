/**
 * @file json_writer.h
 * @brief DTO-to-JSON serialization (DOM construction and direct string dumping).
 */
#ifndef KALE_HTTP_JSON_WRITER_H
#define KALE_HTTP_JSON_WRITER_H

#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include <kale/http/field_desc.h>
#include <kale/http/json.h>

namespace kale {

enum class OptionalNullPolicy {
    Omit,
    EmitNull
};

struct JsonWriteOptions {
    OptionalNullPolicy optional_null{OptionalNullPolicy::Omit};
};

// ============================================================================
// DOM construction: DTO -> J
// ============================================================================

template <typename DTO, typename J = Json>
    requires HasKaleFields<DTO>
J to_json(const DTO &dto, JsonWriteOptions opts = {});

template <typename J>
inline J make_json_object() {
    if constexpr (requires { { J::object() } -> std::convertible_to<J>; }) {
        return J::object();
    } else if constexpr (requires { typename J::Object; J(typename J::Object{}); }) {
        return J(typename J::Object{});
    } else {
        static_assert(!sizeof(J), "JSON type J must support J::object() or J::Object");
    }
}

template <typename J>
inline J make_json_array() {
    if constexpr (requires { { J::array() } -> std::convertible_to<J>; }) {
        return J::array();
    } else if constexpr (requires { typename J::Array; J(typename J::Array{}); }) {
        return J(typename J::Array{});
    } else {
        static_assert(!sizeof(J), "JSON type J must support J::array() or J::Array");
    }
}

template <typename J>
inline void set_json_field(J &obj, std::string_view key, J &&val) {
    if constexpr (requires(J &o, std::string_view k, J v) { o.push_back(k, std::move(v)); }) {
        obj.push_back(key, std::move(val));
    } else if constexpr (requires(J &o, std::string_view k, J v) { o[k] = std::move(v); }) {
        obj[key] = std::move(val);
    } else if constexpr (requires(J &o, std::string k, J v) { o[k] = std::move(v); }) {
        obj[std::string(key)] = std::move(val);
    } else {
        static_assert(!sizeof(J), "JSON type J must support object key insertion");
    }
}

template <typename J>
inline void push_json_array(J &arr, J &&val) {
    if constexpr (requires(J &a, J v) { a.push_back(std::move(v)); }) {
        arr.push_back(std::move(val));
    } else {
        static_assert(!sizeof(J), "JSON type J must support array push_back");
    }
}

/**
 * @brief True for values that need no DOM node of their own beyond the leaf scalar.
 */
template <typename U>
inline constexpr bool is_json_scalar_v =
    std::same_as<U, bool> ||
    std::same_as<U, std::nullptr_t> ||
    std::same_as<U, std::string> ||
    std::same_as<U, const char *> ||
    std::same_as<U, char *> ||
    is_string_view_v<U> ||
    (std::integral<U> && !std::same_as<U, bool>) ||
    std::floating_point<U>;

/**
 * @brief Build the leaf DOM node for a scalar value.
 */
template <typename J, typename U>
inline J make_scalar_node(const U &val) {
    if constexpr (std::same_as<U, std::nullptr_t>) {
        return J(nullptr);
    } else if constexpr (std::same_as<U, const char *> || std::same_as<U, char *>) {
        if (val == nullptr) {
            return J(nullptr);
        }
        if constexpr (std::constructible_from<J, std::string_view>) {
            return J(std::string_view(val));
        } else {
            return J(std::string(val));
        }
    } else if constexpr (is_string_view_v<U>) {
        if constexpr (std::constructible_from<J, std::string_view>) {
            return J(std::string_view(val));
        } else {
            return J(std::string(val));
        }
    } else {
        return J(val);
    }
}

// Insert a scalar field, letting the JSON type store it directly when it can
// (kale::Json does for bool/arithmetic/string/string_view/const char*/nullptr),
// so no temporary J wrapper node is allocated.
template <typename J, typename U>
inline void set_scalar_field(J &obj, std::string_view key, const U &val) {
    if constexpr (std::same_as<U, const char *> || std::same_as<U, char *>) {
        if (val == nullptr) {
            set_json_field(obj, key, J(nullptr));
            return;
        }
    }
    if constexpr (requires(J &o, std::string_view k, const U &v) { o.push_back(k, v); }) {
        obj.push_back(key, val);
    } else {
        set_json_field(obj, key, make_scalar_node<J>(val));
    }
}

template <typename J, typename U>
inline void set_scalar_element(J &arr, const U &val) {
    if constexpr (std::same_as<U, const char *> || std::same_as<U, char *>) {
        if (val == nullptr) {
            push_json_array(arr, J(nullptr));
            return;
        }
    }
    if constexpr (requires(J &a, const U &v) { a.push_back(v); }) {
        arr.push_back(val);
    } else {
        push_json_array(arr, make_scalar_node<J>(val));
    }
}

template <typename J, typename T>
inline J to_json_value(const T &val, JsonWriteOptions opts) {
    using U = std::remove_cvref_t<T>;

    if constexpr (std::same_as<U, J>) {
        return val;
    } else if constexpr (is_json_scalar_v<U>) {
        return make_scalar_node<J>(val);
    } else if constexpr (is_optional_v<U>) {
        if (val.has_value()) {
            return to_json_value<J>(*val, opts);
        }
        return J(nullptr);
    } else if constexpr (is_vector_v<U>) {
        J arr = make_json_array<J>();
        for (const auto &elem : val) {
            using ElemT = std::remove_cvref_t<decltype(elem)>;
            if constexpr (is_json_scalar_v<ElemT>) {
                set_scalar_element<J>(arr, elem);
            } else {
                push_json_array(arr, to_json_value<J>(elem, opts));
            }
        }
        return arr;
    } else if constexpr (HasKaleFields<U>) {
        return to_json<U, J>(val, opts);
    } else {
        static_assert(!sizeof(U), "Unsupported field type for kale::to_json");
    }
}

template <typename J, typename Field, typename Value>
inline void write_field(J &obj, const Field &field, const Value &val, JsonWriteOptions opts) {
    using U = std::remove_cvref_t<Value>;
    if constexpr (is_optional_v<U>) {
        if (!val.has_value()) {
            if (opts.optional_null == OptionalNullPolicy::EmitNull) {
                set_json_field(obj, field.key, J(nullptr));
            }
            return;
        }
        using Inner = std::remove_cvref_t<decltype(*val)>;
        if constexpr (is_json_scalar_v<Inner>) {
            set_scalar_field<J>(obj, field.key, *val);
        } else {
            set_json_field(obj, field.key, to_json_value<J>(*val, opts));
        }
    } else if constexpr (is_json_scalar_v<U>) {
        set_scalar_field<J>(obj, field.key, val);
    } else {
        set_json_field(obj, field.key, to_json_value<J>(val, opts));
    }
}

template <typename DTO, typename J>
    requires HasKaleFields<DTO>
inline J to_json(const DTO &dto, JsonWriteOptions opts) {
    J obj = make_json_object<J>();
    const auto &fields = dto.__kale_fields();
    std::apply(
        [&](const auto &...field) {
            (write_field<J>(obj, field, dto.*(field.member), opts), ...);
        },
        fields);
    return obj;
}

// ============================================================================
// Direct serialization: DTO -> std::string (no intermediate DOM)
// ============================================================================

inline void dump_json_indent(std::string &out, int spaces, int level) {
    if (spaces <= 0) {
        return;
    }
    out.push_back('\n');
    out.append(static_cast<std::size_t>(spaces) * static_cast<std::size_t>(level), ' ');
}

inline void dump_json_escaped(std::string &out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    std::size_t run_start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        const char *escape = nullptr;
        switch (ch) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }
        if (escape != nullptr) {
            if (i > run_start) {
                out.append(text.data() + run_start, i - run_start);
            }
            out.append(escape);
            run_start = i + 1;
        } else if (ch < 0x20) {
            if (i > run_start) {
                out.append(text.data() + run_start, i - run_start);
            }
            const char unicode_escape[6] = {
                '\\', 'u', '0', '0', kHex[ch >> 4], kHex[ch & 0x0F]};
            out.append(unicode_escape, sizeof(unicode_escape));
            run_start = i + 1;
        }
    }
    if (text.size() > run_start) {
        out.append(text.data() + run_start, text.size() - run_start);
    }
    out.push_back('"');
}

template <typename U>
inline void dump_json_number(std::string &out, U value) {
    if constexpr (std::integral<U>) {
        char buf[32];
        const auto result = std::to_chars(buf, buf + sizeof(buf), value);
        if (result.ec != std::errc{}) {
            throw std::runtime_error("kale::dump_json: integer formatting failed");
        }
        out.append(buf, result.ptr);
    } else {
        static_assert(std::floating_point<U>, "dump_json number must be integral or floating point");
        if (!std::isfinite(value)) {
            throw std::invalid_argument("kale::dump_json: non-finite floating-point value");
        }
        char buf[64];
        const auto result = std::to_chars(buf,
                                          buf + sizeof(buf),
                                          value,
                                          std::chars_format::general,
                                          std::numeric_limits<U>::max_digits10);
        if (result.ec != std::errc{}) {
            throw std::runtime_error("kale::dump_json: floating-point formatting failed");
        }
        out.append(buf, result.ptr);
    }
}

template <typename RawJ>
inline void dump_json_raw(std::string &out, const RawJ &value, int spaces, int level) {
    if (spaces > 0) {
        if constexpr (requires(const RawJ &j, int s) { { j.dump(s) } -> std::convertible_to<std::string>; }) {
            std::string s = value.dump(spaces);
            if (level > 0 && s.find('\n') != std::string::npos) {
                const std::size_t indent = static_cast<std::size_t>(spaces) * static_cast<std::size_t>(level);
                std::size_t pos = 0;
                while (pos < s.size()) {
                    std::size_t nl = s.find('\n', pos);
                    if (nl == std::string::npos) {
                        out.append(s, pos, s.size() - pos);
                        break;
                    }
                    out.append(s, pos, nl - pos + 1);
                    out.append(indent, ' ');
                    pos = nl + 1;
                }
            } else {
                out.append(s);
            }
            return;
        }
    }
    if constexpr (requires(const RawJ &j) { { j.dump() } -> std::convertible_to<std::string>; }) {
        out.append(value.dump());
    } else if constexpr (requires(const RawJ &j, int s) { { j.dump(s) } -> std::convertible_to<std::string>; }) {
        out.append(value.dump(0));
    } else {
        static_assert(!sizeof(RawJ), "JSON type J must support dump()");
    }
}

template <typename J, typename U>
inline void dump_json_value(std::string &out,
                            const U &value,
                            int spaces,
                            int level,
                            JsonWriteOptions opts);

template <typename J, typename DTO, typename Field>
inline void dump_json_field(std::string &out,
                            const DTO &dto,
                            const Field &field,
                            bool &first,
                            int spaces,
                            int level,
                            JsonWriteOptions opts) {
    using Member = std::remove_cvref_t<decltype(dto.*(field.member))>;
    const auto &value = dto.*(field.member);
    if constexpr (is_optional_v<Member>) {
        if (!value.has_value() && opts.optional_null == OptionalNullPolicy::Omit) {
            return;
        }
    }
    if (!first) {
        out.push_back(',');
    }
    dump_json_indent(out, spaces, level + 1);
    first = false;
    dump_json_escaped(out, field.key);
    if (spaces > 0) {
        out.append(": ");
    } else {
        out.push_back(':');
    }
    dump_json_value<J>(out, value, spaces, level + 1, opts);
}

template <typename J, typename DTO>
    requires HasKaleFields<DTO>
inline void dump_json_object(std::string &out,
                             const DTO &dto,
                             int spaces,
                             int level,
                             JsonWriteOptions opts) {
    out.push_back('{');
    bool first = true;
    const auto &fields = dto.__kale_fields();
    std::apply(
        [&](const auto &...field) {
            (dump_json_field<J>(out, dto, field, first, spaces, level, opts), ...);
        },
        fields);
    if (spaces > 0 && !first) {
        dump_json_indent(out, spaces, level);
    }
    out.push_back('}');
}

template <typename J, typename U>
inline void dump_json_value(std::string &out,
                            const U &value,
                            int spaces,
                            int level,
                            JsonWriteOptions opts) {
    using V = std::remove_cvref_t<U>;
    if constexpr (std::same_as<V, bool>) {
        out.append(value ? "true" : "false");
    } else if constexpr ((std::integral<V> && !std::same_as<V, bool>) || std::floating_point<V>) {
        dump_json_number(out, value);
    } else if constexpr (std::same_as<V, std::string> || is_string_view_v<V>) {
        dump_json_escaped(out, std::string_view(value));
    } else if constexpr (std::same_as<V, const char *> || std::same_as<V, char *>) {
        if (value == nullptr) {
            out.append("null");
        } else {
            dump_json_escaped(out, std::string_view(value));
        }
    } else if constexpr (std::same_as<V, std::nullptr_t>) {
        out.append("null");
    } else if constexpr (is_optional_v<V>) {
        if (value.has_value()) {
            dump_json_value<J>(out, *value, spaces, level, opts);
        } else {
            out.append("null");
        }
    } else if constexpr (is_vector_v<V>) {
        out.push_back('[');
        bool first = true;
        for (const auto &elem : value) {
            if (!first) {
                out.push_back(',');
            }
            dump_json_indent(out, spaces, level + 1);
            first = false;
            dump_json_value<J>(out, elem, spaces, level + 1, opts);
        }
        if (spaces > 0 && !first) {
            dump_json_indent(out, spaces, level);
        }
        out.push_back(']');
    } else if constexpr (HasKaleFields<V>) {
        dump_json_object<J>(out, value, spaces, level, opts);
    } else if constexpr (std::same_as<V, J>) {
        dump_json_raw(out, value, spaces, level);
    } else if constexpr (requires(const V &v) { { v.dump() } -> std::convertible_to<std::string>; }) {
        dump_json_raw(out, value, spaces, level);
    } else {
        static_assert(!sizeof(V), "Unsupported field type for kale::dump_json");
    }
}

/**
 * @brief Serialize a DTO directly into a JSON string, without building a DOM.
 * @param spaces Indentation width; 0 (or negative) produces compact JSON.
 */
template <typename DTO, typename J = Json>
    requires HasKaleFields<DTO>
inline std::string dump_json(const DTO &dto, int spaces = 0, JsonWriteOptions opts = {}) {
    std::string out;
    out.reserve(128);
    dump_json_object<J>(out, dto, spaces, 0, opts);
    return out;
}

/**
 * @brief Convenience overload: compact serialization with explicit write options.
 */
template <typename DTO, typename J = Json>
    requires HasKaleFields<DTO>
inline std::string dump_json(const DTO &dto, JsonWriteOptions opts) {
    return dump_json<DTO, J>(dto, 0, opts);
}

} // namespace kale

#endif // KALE_HTTP_JSON_WRITER_H
