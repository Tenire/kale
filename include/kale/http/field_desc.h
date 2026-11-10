/**
 * @file field_desc.h
 * @brief Declarative field descriptors, DTO macros, and reflection traits for Kale.
 */
#ifndef KALE_HTTP_FIELD_DESC_H
#define KALE_HTTP_FIELD_DESC_H

#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace kale {

template <typename T>
struct is_string_view : std::false_type {};

template <typename CharT, typename Traits>
struct is_string_view<std::basic_string_view<CharT, Traits>> : std::true_type {};

template <typename T>
inline constexpr bool is_string_view_v = is_string_view<std::remove_cvref_t<T>>::value;

template <typename T>
struct is_optional : std::false_type {};

template <typename U>
struct is_optional<std::optional<U>> : std::true_type {
    using value_type = U;
};

template <typename T>
inline constexpr bool is_optional_v = is_optional<std::remove_cvref_t<T>>::value;

template <typename T>
struct is_vector : std::false_type {};

template <typename U>
struct is_vector<std::vector<U>> : std::true_type {
    using value_type = U;
};

template <typename T>
inline constexpr bool is_vector_v = is_vector<std::remove_cvref_t<T>>::value;

template <typename Class, typename Member, typename... Rules>
struct FieldDesc {
    std::string_view key;
    Member Class::*member;
    std::tuple<Rules...> rules{};

    using class_type = Class;
    using member_type = Member;
    using rules_type = std::tuple<Rules...>;

    constexpr const std::tuple<Rules...> &constraints() const noexcept {
        return rules;
    }
};

template <typename Class, typename Member>
FieldDesc(std::string_view, Member Class::*) -> FieldDesc<Class, Member>;

template <typename Class, typename Member, typename... Rules>
FieldDesc(std::string_view, Member Class::*, std::tuple<Rules...>) -> FieldDesc<Class, Member, Rules...>;

template <typename T>
concept HasKaleFields = requires(const T &t) {
    t.__kale_fields();
};

// DTO declarative field binding macros.
#define KALE_FIELD(field, ...) \
    ::kale::FieldDesc{#field, &__kale_self::field, std::make_tuple(__VA_ARGS__)}

#define KALE_BIND(...) \
    const auto &__kale_fields() const { \
        using __kale_self = std::remove_cvref_t<decltype(*this)>; \
        static const auto fields = std::make_tuple(__VA_ARGS__); \
        return fields; \
    }

} // namespace kale

#endif // KALE_HTTP_FIELD_DESC_H
