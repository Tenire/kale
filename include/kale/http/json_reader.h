/**
 * @file json_reader.h
 * @brief Generic JSON reader traits, field descriptors, and recursive binding engine.
 */
#ifndef KALE_HTTP_JSON_READER_H
#define KALE_HTTP_JSON_READER_H

#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <kale/http/field_desc.h>
#include <kale/core/http_error.h>

namespace kale {

/**
 * @brief Exception thrown on JSON binding failure with field path, expected type, and reason.
 * Maps to HTTP 400 Bad Request.
 */
class JsonBindError : public JsonInputError {
public:
    JsonBindError(std::string field, std::string expected, std::string reason)
        : JsonInputError("JSON bind error at '" + field + "': " + reason + " (expected " + expected + ")"),
          field_(std::move(field)),
          expected_(std::move(expected)),
          reason_(std::move(reason)) {
        with_code("JSON_BIND_ERROR");
        with_detail(field_, reason_ + " (expected " + expected_ + ")");
    }

    const std::string &field() const noexcept { return field_; }
    const std::string &expected() const noexcept { return expected_; }
    const std::string &reason() const noexcept { return reason_; }

private:
    std::string field_;
    std::string expected_;
    std::string reason_;
};

namespace rules {
struct RequiredRule;
}

struct JsonNumber {
    enum class Kind {
        Int64,
        Uint64,
        Float64
    };

    Kind kind{Kind::Float64};
    union {
        int64_t i64{0};
        uint64_t u64;
        double f64;
    };

    static JsonNumber from_int64(int64_t val) noexcept {
        JsonNumber n;
        n.kind = Kind::Int64;
        n.i64 = val;
        return n;
    }

    static JsonNumber from_uint64(uint64_t val) noexcept {
        JsonNumber n;
        n.kind = Kind::Uint64;
        n.u64 = val;
        return n;
    }

    static JsonNumber from_float64(double val) noexcept {
        JsonNumber n;
        n.kind = Kind::Float64;
        n.f64 = val;
        return n;
    }
};

template <typename J>
concept HasParseEventCallback = requires(std::string_view raw) {
    typename J::parse_event_t;
    J::parse_event_t::object_start;
    J::parse_event_t::object_end;
    J::parse_event_t::array_start;
    J::parse_event_t::array_end;
    J::parse_event_t::key;
    J::parse_event_t::value;
    { J::parse(raw, [](int, typename J::parse_event_t, J &) -> bool { return true; }) } -> std::same_as<J>;
};

template <typename J>
concept PreservesDuplicateKeys = requires {
    { J::preserves_duplicate_keys } -> std::convertible_to<bool>;
    requires bool(J::preserves_duplicate_keys);
};

template <typename J>
inline J parse_with_callback(std::string_view raw) {
    struct Frame {
        enum class Type { Object, Array };
        Type type{Type::Object};
        std::string path;
        std::string current_key;
        size_t array_index{0};
        std::unordered_set<std::string> seen_keys;
    };

    std::vector<Frame> frames;

    auto callback = [&frames](int depth, typename J::parse_event_t event, J &parsed) -> bool {
        switch (event) {
        case J::parse_event_t::object_start: {
            std::string path;
            if (depth > 0 && depth - 1 < static_cast<int>(frames.size())) {
                const auto &parent = frames[depth - 1];
                if (parent.type == Frame::Type::Object) {
                    path = parent.path.empty() ? parent.current_key : (parent.path + "." + parent.current_key);
                } else {
                    path = parent.path + "[" + std::to_string(parent.array_index) + "]";
                }
            }
            if (depth >= static_cast<int>(frames.size())) {
                frames.resize(depth + 1);
            }
            frames[depth].type = Frame::Type::Object;
            frames[depth].path = std::move(path);
            frames[depth].current_key.clear();
            frames[depth].seen_keys.clear();
            frames[depth].array_index = 0;
            break;
        }
        case J::parse_event_t::array_start: {
            std::string path;
            if (depth > 0 && depth - 1 < static_cast<int>(frames.size())) {
                const auto &parent = frames[depth - 1];
                if (parent.type == Frame::Type::Object) {
                    path = parent.path.empty() ? parent.current_key : (parent.path + "." + parent.current_key);
                } else {
                    path = parent.path + "[" + std::to_string(parent.array_index) + "]";
                }
            }
            if (depth >= static_cast<int>(frames.size())) {
                frames.resize(depth + 1);
            }
            frames[depth].type = Frame::Type::Array;
            frames[depth].path = std::move(path);
            frames[depth].current_key.clear();
            frames[depth].seen_keys.clear();
            frames[depth].array_index = 0;
            break;
        }
        case J::parse_event_t::key: {
            if (depth > 0 && depth - 1 < static_cast<int>(frames.size())) {
                auto &frame = frames[depth - 1];
                std::string key;
                if constexpr (requires { parsed.is_string(); parsed.template get_ref<const std::string &>(); }) {
                    key = parsed.is_string() ? parsed.template get_ref<const std::string &>() : parsed.dump();
                } else if constexpr (requires { parsed.template get<std::string>(); }) {
                    key = parsed.template get<std::string>();
                } else {
                    key = static_cast<std::string>(parsed);
                }
                if (!frame.seen_keys.insert(key).second) {
                    std::string full_path = frame.path.empty() ? key : (frame.path + "." + key);
                    throw JsonBindError(std::move(full_path), "unique key", "duplicate object key in JSON");
                }
                frame.current_key = key;
            }
            break;
        }
        case J::parse_event_t::value: {
            if (depth > 0 && depth - 1 < static_cast<int>(frames.size())) {
                auto &parent = frames[depth - 1];
                if (parent.type == Frame::Type::Array) {
                    ++parent.array_index;
                }
            }
            break;
        }
        case J::parse_event_t::object_end:
        case J::parse_event_t::array_end: {
            if (depth > 0 && depth - 1 < static_cast<int>(frames.size())) {
                auto &parent = frames[depth - 1];
                if (parent.type == Frame::Type::Array) {
                    ++parent.array_index;
                }
            }
            if (depth < static_cast<int>(frames.size())) {
                frames.resize(depth);
            }
            break;
        }
        }
        return true;
    };

    return J::parse(raw, callback);
}

template <typename J>
inline void check_duplicate_keys_dom(const J &node, std::string_view path) {
    if (node.is_object()) {
        std::unordered_set<std::string> seen;
        seen.reserve(node.size());
        const auto end_it = node.end();
        for (auto it = node.begin(); it != end_it; ++it) {
            std::string key;
            if constexpr (requires { it->key(); }) {
                key = std::string(it->key());
            } else if constexpr (requires { it.key(); }) {
                key = std::string(it.key());
            } else if constexpr (requires { it->first; }) {
                key = std::string(it->first);
            } else {
                static_assert(!sizeof(J), "Cannot extract object key from iterator in check_duplicate_keys_dom");
            }
            if (!seen.insert(key).second) {
                std::string dup_field_path = path.empty() ? key : (std::string(path) + "." + key);
                throw JsonBindError(std::move(dup_field_path), "unique key", "duplicate object key in JSON");
            }
            if constexpr (requires { it->is_object(); it->is_array(); }) {
                if (it->is_object() || it->is_array()) {
                    std::string child_path = path.empty() ? key : (std::string(path) + "." + key);
                    check_duplicate_keys_dom(*it, child_path);
                }
            } else if constexpr (requires { it->second.is_object(); }) {
                if (it->second.is_object() || it->second.is_array()) {
                    std::string child_path = path.empty() ? key : (std::string(path) + "." + key);
                    check_duplicate_keys_dom(it->second, child_path);
                }
            }
        }
    } else if (node.is_array()) {
        const auto end_it = node.end();
        int idx = 0;
        for (auto it = node.begin(); it != end_it; ++it, ++idx) {
            if constexpr (requires { it->is_object(); it->is_array(); }) {
                if (it->is_object() || it->is_array()) {
                    std::string elem_path = std::string(path) + "[" + std::to_string(idx) + "]";
                    check_duplicate_keys_dom(*it, elem_path);
                }
            } else if constexpr (requires { it->second.is_object(); }) {
                if (it->second.is_object() || it->second.is_array()) {
                    std::string elem_path = std::string(path) + "[" + std::to_string(idx) + "]";
                    check_duplicate_keys_dom(it->second, elem_path);
                }
            }
        }
    }
}

template <typename J>
struct JsonReader {
    struct Handle {
        const J *ref{nullptr};
        std::optional<J> storage;

        Handle() noexcept = default;
        explicit Handle(const J &r) noexcept : ref(&r), storage(std::nullopt) {}
        explicit Handle(J &&val) noexcept(std::is_nothrow_move_constructible_v<J>)
            : ref(nullptr), storage(std::move(val)) {}

        Handle(const Handle &) = delete;
        Handle &operator=(const Handle &) = delete;
        Handle(Handle &&) noexcept(std::is_nothrow_move_constructible_v<J>) = default;
        Handle &operator=(Handle &&) noexcept(std::is_nothrow_move_assignable_v<J>) = default;

        bool is_valid() const noexcept {
            return ref != nullptr || storage.has_value();
        }

        const J &node() const noexcept {
            return storage.has_value() ? *storage : *ref;
        }

        const J *operator->() const noexcept { return &node(); }
        const J &operator*() const noexcept { return node(); }
    };

    J doc;

    static JsonReader parse_for_binding(std::string_view raw) {
        if (raw.empty()) {
            throw JsonBindError("_root", "object", "empty request body");
        }

        static_assert(HasParseEventCallback<J> || PreservesDuplicateKeys<J>,
            "Duplicate key validation cannot be guaranteed for J: the JSON type does not support "
            "parse event callbacks and does not declare `static constexpr bool preserves_duplicate_keys = true;`. "
            "Provide a parse callback, declare preserves_duplicate_keys, or specialize JsonReader<J>.");

        J parsed_doc;
        try {
            if constexpr (HasParseEventCallback<J>) {
                parsed_doc = parse_with_callback<J>(raw);
            } else {
                parsed_doc = J::parse(raw);
            }
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const JsonBindError &) {
            throw;
        } catch (const std::exception &e) {
            throw JsonBindError("_root", "valid JSON", std::string("malformed JSON syntax: ") + e.what());
        }

        JsonReader reader;
        reader.doc = std::move(parsed_doc);

        if (!reader.is_object(reader.root())) {
            throw JsonBindError("_root", "object", "root JSON value must be an object");
        }

        if constexpr (!HasParseEventCallback<J> && PreservesDuplicateKeys<J>) {
            check_duplicate_keys_dom(reader.doc, "");
        }

        return reader;
    }

    Handle root() const noexcept {
        return Handle{doc};
    }

    bool is_object(const Handle &h) const noexcept {
        return h.is_valid() && h.node().is_object();
    }

    bool is_array(const Handle &h) const noexcept {
        return h.is_valid() && h.node().is_array();
    }

    bool is_null(const Handle &h) const noexcept {
        return !h.is_valid() || h.node().is_null();
    }

    bool is_bool(const Handle &h) const noexcept {
        if (!h.is_valid()) return false;
        if constexpr (requires { h.node().is_boolean(); }) {
            return h.node().is_boolean();
        } else {
            return h.node().is_bool();
        }
    }

    bool is_string(const Handle &h) const noexcept {
        return h.is_valid() && h.node().is_string();
    }

    bool is_number(const Handle &h) const noexcept {
        return h.is_valid() && h.node().is_number();
    }

    bool get_bool(const Handle &h) const {
        if (!h.is_valid()) return false;
        if constexpr (requires { h.node().template get<bool>(); }) {
            return h.node().template get<bool>();
        } else if constexpr (requires { h.node().get_bool(); }) {
            return h.node().get_bool();
        } else {
            return static_cast<bool>(h.node());
        }
    }

    std::string get_string(const Handle &h) const {
        if (!h.is_valid()) return {};
        if constexpr (requires { h.node().template get<std::string>(); }) {
            return h.node().template get<std::string>();
        } else if constexpr (requires { h.node().get_string(); }) {
            return h.node().get_string();
        } else {
            return static_cast<std::string>(h.node());
        }
    }

    JsonNumber get_number(const Handle &h) const {
        if (!h.is_valid() || !is_number(h)) {
            return JsonNumber::from_float64(0.0);
        }
        const auto &n = h.node();

        if constexpr (requires { n.is_number_unsigned(); n.is_number_integer(); }) {
            if (n.is_number_unsigned()) {
                static_assert(requires { n.template get<uint64_t>(); },
                    "JSON type advertises unsigned integers but get<uint64_t>() is unsupported");
                return JsonNumber::from_uint64(n.template get<uint64_t>());
            }
            if (n.is_number_integer()) {
                static_assert(requires { n.template get<int64_t>(); },
                    "JSON type advertises signed integers but get<int64_t>() is unsupported");
                return JsonNumber::from_int64(n.template get<int64_t>());
            }
            if constexpr (requires { n.is_number_float(); }) {
                if (n.is_number_float()) {
                    return JsonNumber::from_float64(n.template get<double>());
                }
            }
            return JsonNumber::from_float64(n.template get<double>());
        } else if constexpr (requires { n.type(); n.type() == J::value_t::number_unsigned; }) {
            if (n.type() == J::value_t::number_unsigned) {
                return JsonNumber::from_uint64(n.template get<uint64_t>());
            }
            if (n.type() == J::value_t::number_integer) {
                return JsonNumber::from_int64(n.template get<int64_t>());
            }
            return JsonNumber::from_float64(n.template get<double>());
        } else {
            return JsonNumber::from_float64(n.template get<double>());
        }
    }

    bool has_member(const Handle &h, std::string_view key) const {
        if (!h.is_valid() || !is_object(h)) return false;
        const auto &n = h.node();
        if constexpr (requires { n.contains(key); }) {
            return n.contains(key);
        } else if constexpr (requires { n.has(key); }) {
            return n.has(key);
        } else if constexpr (requires { n.find(key) != n.end(); }) {
            return n.find(key) != n.end();
        } else {
            static_assert(!sizeof(J), "JSON type does not support key lookup via contains(), has(), or find()");
        }
    }

    std::optional<Handle> get_member(const Handle &h, std::string_view key) const {
        if (!has_member(h, key)) {
            return std::nullopt;
        }
        const auto &n = h.node();
        if constexpr (requires { n.find(key); }) {
            auto it = n.find(key);
            if (it == n.end()) {
                return std::nullopt;
            }
            decltype(auto) val = *it;
            if constexpr (std::is_lvalue_reference_v<decltype(val)>) {
                return Handle{val};
            } else {
                return Handle{std::move(val)};
            }
        } else if constexpr (requires { n[key]; }) {
            decltype(auto) val = n[key];
            if constexpr (std::is_lvalue_reference_v<decltype(val)>) {
                return Handle{val};
            } else {
                return Handle{std::move(val)};
            }
        } else if constexpr (requires { n[std::string(key)]; }) {
            decltype(auto) val = n[std::string(key)];
            if constexpr (std::is_lvalue_reference_v<decltype(val)>) {
                return Handle{val};
            } else {
                return Handle{std::move(val)};
            }
        } else {
            static_assert(!sizeof(J), "JSON type does not support member access via find() or operator[]");
        }
    }

    size_t array_size(const Handle &h) const noexcept {
        return (h.is_valid() && is_array(h)) ? h.node().size() : 0;
    }

    Handle array_at(const Handle &h, size_t index) const {
        if (!h.is_valid() || !is_array(h) || index >= h.node().size()) {
            return Handle{};
        }
        const auto &n = h.node();
        if constexpr (requires { n.at(index); }) {
            decltype(auto) val = n.at(index);
            if constexpr (std::is_lvalue_reference_v<decltype(val)>) {
                return Handle{val};
            } else {
                return Handle{std::move(val)};
            }
        } else if constexpr (requires { n[static_cast<int>(index)]; }) {
            decltype(auto) val = n[static_cast<int>(index)];
            if constexpr (std::is_lvalue_reference_v<decltype(val)>) {
                return Handle{val};
            } else {
                return Handle{std::move(val)};
            }
        } else {
            static_assert(!sizeof(J), "JSON type does not support array element indexing via at() or operator[]");
        }
    }

    J to_json(const Handle &h) const {
        return h.is_valid() ? h.node() : J{};
    }
};
template <typename T>
    requires (std::integral<T> || std::floating_point<T>) && (!std::same_as<T, bool>)
inline void extract_numeric(const JsonNumber &num, std::string_view path, T &target) {
    if constexpr (std::integral<T>) {
        if constexpr (std::is_signed_v<T>) {
            if (num.kind == JsonNumber::Kind::Int64) {
                if (num.i64 < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
                    num.i64 > static_cast<int64_t>(std::numeric_limits<T>::max())) {
                    throw JsonBindError(std::string(path), "integer", "numeric value out of range");
                }
                target = static_cast<T>(num.i64);
            } else if (num.kind == JsonNumber::Kind::Uint64) {
                if (num.u64 > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
                    throw JsonBindError(std::string(path), "integer", "numeric value out of range");
                }
                target = static_cast<T>(num.u64);
            } else { // Float64
                if (!std::isfinite(num.f64) || std::trunc(num.f64) != num.f64) {
                    throw JsonBindError(std::string(path), "integer", "floating point or non-finite value not allowed for integer");
                }
                constexpr double min_val = static_cast<double>(std::numeric_limits<T>::min());
                constexpr double max_val = static_cast<double>(std::numeric_limits<T>::max());
                constexpr bool max_rounds_up = std::numeric_limits<T>::digits > std::numeric_limits<double>::digits;
                if (num.f64 < min_val || (max_rounds_up ? num.f64 >= max_val : num.f64 > max_val)) {
                    throw JsonBindError(std::string(path), "integer", "numeric value out of range");
                }
                target = static_cast<T>(num.f64);
            }
        } else { // Unsigned integral
            if (num.kind == JsonNumber::Kind::Int64) {
                if (num.i64 < 0 ||
                    static_cast<uint64_t>(num.i64) > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
                    throw JsonBindError(std::string(path), "unsigned integer", "numeric value out of range");
                }
                target = static_cast<T>(num.i64);
            } else if (num.kind == JsonNumber::Kind::Uint64) {
                if (num.u64 > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
                    throw JsonBindError(std::string(path), "unsigned integer", "numeric value out of range");
                }
                target = static_cast<T>(num.u64);
            } else { // Float64
                if (!std::isfinite(num.f64) || std::trunc(num.f64) != num.f64) {
                    throw JsonBindError(std::string(path), "unsigned integer", "floating point or non-finite value not allowed for integer");
                }
                constexpr double max_val = static_cast<double>(std::numeric_limits<T>::max());
                constexpr bool max_rounds_up = std::numeric_limits<T>::digits > std::numeric_limits<double>::digits;
                if (num.f64 < 0.0 || (max_rounds_up ? num.f64 >= max_val : num.f64 > max_val)) {
                    throw JsonBindError(std::string(path), "unsigned integer", "numeric value out of range");
                }
                target = static_cast<T>(num.f64);
            }
        }
    } else { // Floating point (float, double)
        if (num.kind == JsonNumber::Kind::Int64) {
            target = static_cast<T>(num.i64);
        } else if (num.kind == JsonNumber::Kind::Uint64) {
            target = static_cast<T>(num.u64);
        } else { // Float64
            if (!std::isfinite(num.f64)) {
                throw JsonBindError(std::string(path), "float", "non-finite floating point value");
            }
            if constexpr (std::numeric_limits<T>::max() < std::numeric_limits<double>::max()) {
                if (num.f64 < static_cast<double>(std::numeric_limits<T>::lowest()) ||
                    num.f64 > static_cast<double>(std::numeric_limits<T>::max())) {
                    throw JsonBindError(std::string(path), "float", "floating point value out of range");
                }
            }
            target = static_cast<T>(num.f64);
        }
    }
}

template <typename T, typename J>
void bind_value(const JsonReader<J> &reader,
                const typename JsonReader<J>::Handle &handle,
                std::string_view path,
                T &target);

template <typename DTO, typename J>
void bind_dto_fields(const JsonReader<J> &reader,
                     const typename JsonReader<J>::Handle &parent_handle,
                     std::string_view parent_path,
                     DTO &target);


template <typename R>
struct is_required_rule : std::false_type {};

template <>
struct is_required_rule<rules::RequiredRule> : std::true_type {};

template <typename R>
inline constexpr bool is_required_rule_v = is_required_rule<std::remove_cvref_t<R>>::value;

template <typename Tuple>
struct tuple_has_required;

template <typename... Ts>
struct tuple_has_required<std::tuple<Ts...>> {
    static constexpr bool value = (is_required_rule_v<Ts> || ...);
};

template <typename Tuple>
inline constexpr bool tuple_has_required_v = tuple_has_required<std::remove_cvref_t<Tuple>>::value;

template <typename Desc>
struct desc_rules_type {
    using type = std::tuple<>;
};

template <typename Desc>
    requires requires { typename Desc::rules_type; }
struct desc_rules_type<Desc> {
    using type = typename Desc::rules_type;
};

template <typename Desc>
using desc_rules_type_t = typename desc_rules_type<Desc>::type;

template <typename Val, typename Rule>
inline void apply_single_constraint(std::string_view path, const Val &val, const Rule &rule) {
    if constexpr (is_required_rule_v<Rule>) {
        // Presence and non-null handled explicitly in bind_single_field
        return;
    } else if constexpr (requires { rule.check(val); }) {
        if (!rule.check(val)) {
            throw JsonBindError(std::string(path), rule.expected(), rule.reason());
        }
    } else if constexpr (requires { rule(val); }) {
        auto err = rule(val);
        if (err.has_value()) {
            throw JsonBindError(std::string(path), "valid value", *err);
        }
    }
}

template <typename Val, typename Tuple>
inline void apply_constraints(std::string_view path, const Val &val, const Tuple &rules) {
    if constexpr (std::tuple_size_v<std::remove_cvref_t<Tuple>> > 0) {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (apply_single_constraint(path, val, std::get<Is>(rules)), ...);
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<Tuple>>>{});
    }
}

template <typename J, typename Desc, typename Target>
inline void bind_single_field(const JsonReader<J> &reader,
                              const typename JsonReader<J>::Handle &parent_handle,
                              std::string_view parent_path,
                              const Desc &desc,
                              Target &target) {
    std::string_view key = desc.key;
    auto &member = target.*(desc.member);
    using MemberT = std::remove_cvref_t<decltype(member)>;

    std::string field_path;
    if (parent_path.empty()) {
        field_path = std::string(key);
    } else {
        field_path.reserve(parent_path.size() + 1 + key.size());
        field_path.append(parent_path);
        field_path.push_back('.');
        field_path.append(key);
    }

    auto member_opt = reader.get_member(parent_handle, key);

    if constexpr (is_optional_v<MemberT>) {
        using InnerT = typename is_optional<MemberT>::value_type;
        constexpr bool has_required = tuple_has_required_v<desc_rules_type_t<Desc>>;
        if (!member_opt.has_value()) {
            if constexpr (has_required) {
                throw JsonBindError(std::move(field_path), "present", "required field is missing");
            }
            member = std::nullopt;
            return;
        }
        if (reader.is_null(*member_opt)) {
            if constexpr (has_required) {
                throw JsonBindError(std::move(field_path), "non-null", "required field is null");
            }
            member = std::nullopt;
            return;
        }
        InnerT inner{};
        bind_value<InnerT, J>(reader, *member_opt, field_path, inner);
        if constexpr (requires { desc.constraints(); }) {
            apply_constraints(field_path, inner, desc.constraints());
        }
        member = std::move(inner);
    } else {
        if (!member_opt.has_value()) {
            throw JsonBindError(std::move(field_path), "present", "required field is missing");
        }
        if (reader.is_null(*member_opt)) {
            throw JsonBindError(std::move(field_path), "non-null", "required field is null");
        }
        bind_value<MemberT, J>(reader, *member_opt, field_path, member);
        if constexpr (requires { desc.constraints(); }) {
            apply_constraints(field_path, member, desc.constraints());
        }
    }
}

template <typename T, typename J>
void bind_value(const JsonReader<J> &reader,
                const typename JsonReader<J>::Handle &handle,
                std::string_view path,
                T &target) {
    if constexpr (is_string_view_v<T>) {
        static_assert(!sizeof(T),
            "DTO fields of type std::string_view are prohibited: parsed JSON does not "
            "outlive bind_json() and would cause dangling string_view references. Use std::string instead.");
    } else if constexpr (std::is_pointer_v<T>) {
        static_assert(!sizeof(T),
            "Pointer types are prohibited as DTO fields; use owning types like std::string instead");
    } else if constexpr (std::same_as<T, bool>) {
        if (!reader.is_bool(handle)) {
            throw JsonBindError(std::string(path), "boolean", "value is not a boolean");
        }
        target = reader.get_bool(handle);
    } else if constexpr ((std::integral<T> || std::floating_point<T>) && !std::same_as<T, bool>) {
        if (!reader.is_number(handle)) {
            throw JsonBindError(std::string(path),
                                std::is_integral_v<T> ? (std::is_unsigned_v<T> ? "unsigned integer" : "integer") : "number",
                                "value is not a number");
        }
        extract_numeric<T>(reader.get_number(handle), path, target);
    } else if constexpr (std::same_as<T, std::string>) {
        if (!reader.is_string(handle)) {
            throw JsonBindError(std::string(path), "string", "value is not a string");
        }
        target = reader.get_string(handle);
    } else if constexpr (std::same_as<T, J>) {
        target = reader.to_json(handle);
    } else if constexpr (is_optional_v<T>) {
        using ValueT = typename is_optional<T>::value_type;
        if (reader.is_null(handle)) {
            target = std::nullopt;
        } else {
            ValueT inner{};
            bind_value<ValueT, J>(reader, handle, path, inner);
            target = std::move(inner);
        }
    } else if constexpr (is_vector_v<T>) {
        using ElemT = typename is_vector<T>::value_type;
        if (!reader.is_array(handle)) {
            throw JsonBindError(std::string(path), "array", "value is not an array");
        }
        target.clear();
        const size_t sz = reader.array_size(handle);
        target.reserve(sz);
        for (size_t i = 0; i < sz; ++i) {
            std::string elem_path = std::string(path) + "[" + std::to_string(i) + "]";
            auto elem_handle = reader.array_at(handle, i);
            ElemT item{};
            bind_value<ElemT, J>(reader, elem_handle, elem_path, item);
            target.push_back(std::move(item));
        }
    } else if constexpr (HasKaleFields<T>) {
        if (!reader.is_object(handle)) {
            throw JsonBindError(std::string(path), "object", "value is not an object");
        }
        bind_dto_fields<T, J>(reader, handle, path, target);
    } else {
        static_assert(!sizeof(T), "Unsupported type for JSON binding");
    }
}

template <typename DTO, typename J>
void bind_dto_fields(const JsonReader<J> &reader,
                     const typename JsonReader<J>::Handle &parent_handle,
                     std::string_view parent_path,
                     DTO &target) {
    if constexpr (HasKaleFields<DTO>) {
        const auto &fields = target.__kale_fields();
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (bind_single_field<J>(reader, parent_handle, parent_path, std::get<Is>(fields), target), ...);
        }(std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>>{});
    } else {
        static_assert(HasKaleFields<DTO>, "DTO type must provide KALE_BIND(...) reflection");
    }
}

template <typename DTO, typename J>
DTO bind_json_body(std::string_view body) {
    static_assert(!is_string_view_v<DTO>,
                  "DTO type cannot be std::string_view; parsed JSON does not outlive bind_json()");
    static_assert(!std::is_pointer_v<DTO>,
                  "Pointer types are prohibited as DTO fields; use owning types like std::string instead");
    if (body.empty()) {
        throw JsonBindError("_root", "object", "empty request body");
    }

    auto reader = JsonReader<J>::parse_for_binding(body);
    auto root = reader.root();
    if (!reader.is_object(root)) {
        throw JsonBindError("_root", "object", "root JSON value must be an object");
    }

    DTO dto{};
    bind_dto_fields<DTO, J>(reader, root, "", dto);
    return dto;
}


} // namespace kale

#endif // KALE_HTTP_JSON_READER_H