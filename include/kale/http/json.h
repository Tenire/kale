#ifndef KALE_JSON_H
#define KALE_JSON_H

#include "workflow/json_parser.h"

#include <cmath>
#include <limits>
#include <concepts>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kale {

class JsonParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json {
public:
    class Object;
    class Array;

    static constexpr bool preserves_duplicate_keys = true;

    Json();
    Json(std::nullptr_t);
    Json(bool value);
    template <typename T>
        requires (std::integral<T> || std::floating_point<T>) && (!std::same_as<T, bool>)
    Json(T value) : node_(json_value_create(JSON_VALUE_NUMBER, static_cast<double>(value))), parent_(nullptr), allocated_(true) {}
    Json(std::string_view value);
    Json(const std::string &value);
    Json(const char *value);
    Json(const std::vector<std::string> &values);

    static Json parse(std::string_view input);
    static Json parse(FILE *file);

    Json(const Json &other);
    Json &operator=(const Json &other);
    Json(Json &&other) noexcept;
    Json &operator=(Json &&other) noexcept;
    ~Json();

    int type() const noexcept;
    std::string type_str() const;
    bool is_valid() const noexcept { return node_ != nullptr; }
    bool is_null() const noexcept { return is_valid() && json_value_type(node_) == JSON_VALUE_NULL; }
    bool is_number() const noexcept { return is_valid() && json_value_type(node_) == JSON_VALUE_NUMBER; }
    bool is_boolean() const noexcept {
        if (!node_) return false;
        const int value_type = json_value_type(node_);
        return value_type == JSON_VALUE_TRUE || value_type == JSON_VALUE_FALSE;
    }
    bool is_string() const noexcept { return is_valid() && json_value_type(node_) == JSON_VALUE_STRING; }
    bool is_object() const noexcept { return is_valid() && json_value_type(node_) == JSON_VALUE_OBJECT; }
    bool is_array() const noexcept { return is_valid() && json_value_type(node_) == JSON_VALUE_ARRAY; }

    size_t size() const noexcept;
    bool empty() const noexcept;
    void clear();
    bool has(std::string_view key) const noexcept;
    void erase(std::string_view key);
    void erase(int index);

    template <typename T> requires std::same_as<T, bool>
    T get() const { return is_boolean() && json_value_type(node_) == JSON_VALUE_TRUE; }
    template <typename T>
        requires (std::integral<T> || std::floating_point<T>) && (!std::same_as<T, bool>)
    T get() const {
        if (!is_number()) return T{};
        const double value = json_value_number(node_);
        if constexpr (std::integral<T>) {
            const double upper = std::ldexp(1.0, std::numeric_limits<T>::digits);
            const double lower = std::is_signed_v<T> ? -upper : 0.0;
            if (!std::isfinite(value) || std::trunc(value) != value || value < lower || value >= upper)
                return T{};
        } else {
            if (std::isfinite(value) && (value > std::numeric_limits<T>::max() || value < -std::numeric_limits<T>::max()))
                return T{};
        }
        return static_cast<T>(value);
    }
    template <typename T> requires std::same_as<T, std::string>
    T get() const {
        const char *value = is_string() ? json_value_string(node_) : nullptr;
        return value ? std::string(value) : std::string{};
    }
    template <typename T> requires std::same_as<T, std::string_view>
    T get() const {
        const char *value = is_string() ? json_value_string(node_) : nullptr;
        return value ? std::string_view(value) : std::string_view{};
    }
    template <typename T>
    T get_or(T fallback) const {
        bool compatible = true;
        if constexpr (std::same_as<T, bool>) compatible = is_boolean();
        else if constexpr (std::integral<T> || std::floating_point<T>) compatible = is_number();
        else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) compatible = is_string();
        else if constexpr (std::same_as<T, Object>) compatible = is_object();
        else if constexpr (std::same_as<T, Array>) compatible = is_array();
        if (!is_valid() || is_null() || !compatible) return fallback;
        return get<T>();
    }
    template <typename T> requires std::same_as<T, Object>
    T get() const;
    template <typename T> requires std::same_as<T, Array>
    T get() const;
    template <typename T> requires std::same_as<T, std::nullptr_t>
    T get() const { return nullptr; }
    template <typename T>
    operator T() const { return get<T>(); }

    Json operator[](const char *key);
    Json operator[](const char *key) const;
    Json operator[](const std::string &key);
    Json operator[](const std::string &key) const;
    Json operator[](std::string_view key);
    Json operator[](std::string_view key) const;
    Json operator[](int index);
    Json operator[](int index) const;

    Json &operator=(std::nullptr_t);
    Json &operator=(bool value);
    template <typename T>
        requires (std::integral<T> || std::floating_point<T>) && (!std::same_as<T, bool>)
    Json &operator=(T value) {
        assign_number(static_cast<double>(value));
        return *this;
    }
    Json &operator=(const char *value);
    Json &operator=(std::string_view value);
    Json &operator=(const std::string &value);

    template <typename T>
        requires (!std::same_as<std::decay_t<T>, Json> &&
                  !std::same_as<std::decay_t<T>, std::nullptr_t> &&
                  !std::same_as<std::decay_t<T>, bool> &&
                  !std::integral<std::decay_t<T>> &&
                  !std::floating_point<std::decay_t<T>> &&
                  !std::convertible_to<T, std::string_view>)
    Json &operator=(const T &value) {
        if (!parent_) {
            *this = Json(value);
            return *this;
        }
        assign_proxy(Json(value));
        return *this;
    }

    Json &push_back(const Json &value);
    Json &push_back(Json &&value);
    Json &push_back(std::nullptr_t);
    Json &push_back(bool value);
    template <typename T>
        requires (std::integral<T> || std::floating_point<T>) && (!std::same_as<T, bool>)
    Json &push_back(T value) {
        push_back_arr_number(static_cast<double>(value));
        return *this;
    }
    Json &push_back(const char *value);
    Json &push_back(std::string_view value);
    Json &push_back(const std::string &value);

    Json &push_back(std::string_view key, const Json &value);
    Json &push_back(std::string_view key, Json &&value);
    Json &push_back(std::string_view key, std::nullptr_t);
    Json &push_back(std::string_view key, bool value);
    template <typename T>
        requires (std::integral<T> || std::floating_point<T>) && (!std::same_as<T, bool>)
    Json &push_back(std::string_view key, T value) {
        push_back_obj_number(key, static_cast<double>(value));
        return *this;
    }
    Json &push_back(std::string_view key, const char *value);
    Json &push_back(std::string_view key, std::string_view value);
    Json &push_back(std::string_view key, const std::string &value);
    std::string dump(int spaces = 0) const;
    std::string key() const { return parent_key_; }
    const Json &value() const { return *this; }

    class IteratorBase {
    public:
        friend class Json;
        explicit IteratorBase(const json_value_t *value);
        virtual ~IteratorBase();
        IteratorBase(const IteratorBase &other);
        IteratorBase &operator=(const IteratorBase &other);
        IteratorBase(IteratorBase &&other) noexcept;
        IteratorBase &operator=(IteratorBase &&other) noexcept;
        Json &operator*() const;
        Json *operator->() const;
        bool operator==(const IteratorBase &other) const;
        bool operator!=(const IteratorBase &other) const { return !(*this == other); }
    protected:
        const json_value_t *val_{nullptr};
        const char *key_{nullptr};
        Json *json_{nullptr};
    };
    class iterator : public IteratorBase {
    public:
        friend class Json;
        explicit iterator(const json_value_t *value) : IteratorBase(value) {}
        iterator &operator++();
        iterator operator++(int);
    private:
        void set_begin();
        void set_end();
        bool is_end() const;
        void forward();
    };
    class reverse_iterator : public IteratorBase {
    public:
        friend class Json;
        explicit reverse_iterator(const json_value_t *value) : IteratorBase(value) {}
        reverse_iterator &operator++();
        reverse_iterator operator++(int);
    private:
        void set_rbegin();
        void set_rend();
        bool is_rend() const;
        void backward();
    };
    iterator begin() const;
    iterator end() const;
    reverse_iterator rbegin() const;
    reverse_iterator rend() const;
    friend std::ostream &operator<<(std::ostream &out, const Json &value) { return out << value.dump(); }

protected:
    enum class JsonType { Object, Array };
    explicit Json(JsonType type);
    Json(const json_value_t *node, const json_value_t *parent, std::string key);
    bool is_root() const noexcept { return parent_ == nullptr; }
    bool is_placeholder() const noexcept {
        return parent_ != nullptr && (node_ == nullptr || (allocated_ && json_value_type(node_) == JSON_VALUE_NULL));
    }
    bool to_object();
    bool to_array();
    bool can_obj_push_back();
    bool can_arr_push_back();
    void reset() noexcept;
    void set_parent(const json_value_t *parent, std::string key) {
        parent_ = parent;
        parent_key_ = std::move(key);
    }
    Json(json_value_t *node, bool allocated) noexcept
        : node_(node), parent_(nullptr), allocated_(allocated) {}
    void destroy_node() noexcept;
    Json copy() const;
private:
    void assign_number(double value);
    void assign_bool(bool value);
    void assign_null();
    void assign_string(const char *value);
    void assign_string_view(std::string_view value);

    void assign_proxy(const Json &value);
    void assign_proxy(Json &&value);
    void assign_proxy_bool(bool value);
    void assign_proxy_number(double value);
    void assign_proxy_null();
    void assign_proxy_string(const char *value);
    void assign_proxy_string_view(std::string_view value);

    void push_back_obj(std::string_view key, const Json &value);
    void push_back_obj(std::string_view key, Json &&value);
    void push_back_obj_number(std::string_view key, double value);
    void push_back_arr_number(double value);
    void update_arr(const Json &value);
    static void value_convert(const json_value_t *value, int spaces, int depth, std::string *out);
    static void string_convert(const char *value, std::string *out);
    static void number_convert(double value, std::string *out);
    static void array_convert(const json_array_t *value, int spaces, int depth, std::string *out);
    static void array_convert_compact(const json_array_t *value, std::string *out);
    static void object_convert(const json_object_t *value, int spaces, int depth, std::string *out);
    static void object_convert_compact(const json_object_t *value, std::string *out);
protected:
    json_value_t *node_{nullptr};
    const json_value_t *parent_{nullptr};
    bool allocated_{false};
    std::string parent_key_;
};

class Json::Object : public Json {
public:
    Object() : Json(JsonType::Object) {}
    Object(const json_value_t *node, const json_value_t *parent) : Json(node, parent, "") {}
    using pair_type = std::pair<std::string, Json>;
    Object(std::initializer_list<pair_type> values) : Json(JsonType::Object) {
        for (const auto &pair : values) push_back(pair.first, pair.second);
    }
};
class Json::Array : public Json {
public:
    Array() : Json(JsonType::Array) {}
    Array(const json_value_t *node, const json_value_t *parent) : Json(node, parent, "") {}
    Array(std::initializer_list<Json> values) : Json(JsonType::Array) {
        for (const auto &value : values) push_back(value);
    }
};
using JsonObject = Json::Object;
using JsonArray = Json::Array;
template <typename T> requires std::same_as<T, Json::Object>
T Json::get() const { return Json::Object(node_, parent_); }
template <typename T> requires std::same_as<T, Json::Array>
T Json::get() const { return Json::Array(node_, parent_); }
} // namespace kale

#endif // KALE_JSON_H
