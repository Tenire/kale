#ifndef KALE_OPENAPI_H
#define KALE_OPENAPI_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <kale/http/field_desc.h>
#include <kale/http/validator.h>
#include <kale/core/router.h>

namespace kale {

enum class UndocumentedRoutePolicy {
    GenerateDefault,
    RequireExplicit,
    Exclude
};

struct OpenApiConfig {
    std::string title{"Kale Application API"};
    std::string version{"1.0.0"};
    std::string description;
    UndocumentedRoutePolicy undocumented_policy{UndocumentedRoutePolicy::GenerateDefault};
};

struct NumericBound {
    enum class Kind { None, Int64, Uint64, Float64 } kind{Kind::None};
    union {
        int64_t i64{0};
        uint64_t u64;
        double f64;
    };
    bool exclusive{false};

    static NumericBound from_int64(int64_t v, bool excl = false) noexcept {
        NumericBound b;
        b.kind = Kind::Int64;
        b.i64 = v;
        b.exclusive = excl;
        return b;
    }

    static NumericBound from_uint64(uint64_t v, bool excl = false) noexcept {
        NumericBound b;
        b.kind = Kind::Uint64;
        b.u64 = v;
        b.exclusive = excl;
        return b;
    }

    static NumericBound from_float64(double v, bool excl = false) {
        if (!std::isfinite(v)) {
            throw std::invalid_argument("kale::openapi: numeric boundary must be finite");
        }
        NumericBound b;
        b.kind = Kind::Float64;
        b.f64 = v;
        b.exclusive = excl;
        return b;
    }

    double as_double() const noexcept {
        switch (kind) {
            case Kind::Int64: return static_cast<double>(i64);
            case Kind::Uint64: return static_cast<double>(u64);
            case Kind::Float64: return f64;
            default: return 0.0;
        }
    }

    double value_or(double def) const noexcept {
        return kind == Kind::None ? def : as_double();
    }

    bool operator==(double d) const noexcept { return as_double() == d; }
    bool operator==(int64_t v) const noexcept {
        if (kind == Kind::Int64) return i64 == v;
        if (kind == Kind::Uint64) return v >= 0 && u64 == static_cast<uint64_t>(v);
        return f64 == static_cast<double>(v);
    }
    bool operator==(uint64_t v) const noexcept {
        if (kind == Kind::Uint64) return u64 == v;
        if (kind == Kind::Int64) return i64 >= 0 && static_cast<uint64_t>(i64) == v;
        return f64 == static_cast<double>(v);
    }

    std::string dump() const;
};

struct EnumValue {
    enum class Kind { String, Int64, Uint64, Float64, Bool } kind{Kind::String};
    std::string str_val;
    int64_t i64_val{0};
    uint64_t u64_val{0};
    double f64_val{0.0};
    bool bool_val{false};

    static EnumValue from_string(std::string s) {
        EnumValue v;
        v.kind = Kind::String;
        v.str_val = std::move(s);
        return v;
    }

    static EnumValue from_int64(int64_t v) noexcept {
        EnumValue v_val;
        v_val.kind = Kind::Int64;
        v_val.i64_val = v;
        return v_val;
    }

    static EnumValue from_uint64(uint64_t v) noexcept {
        EnumValue v_val;
        v_val.kind = Kind::Uint64;
        v_val.u64_val = v;
        return v_val;
    }

    static EnumValue from_float64(double v) {
        if (!std::isfinite(v)) {
            throw std::invalid_argument("kale::openapi: enum float value must be finite");
        }
        EnumValue v_val;
        v_val.kind = Kind::Float64;
        v_val.f64_val = v;
        return v_val;
    }

    static EnumValue from_bool(bool v) noexcept {
        EnumValue v_val;
        v_val.kind = Kind::Bool;
        v_val.bool_val = v;
        return v_val;
    }

    bool operator==(const EnumValue &other) const {
        if (kind != other.kind) return false;
        switch (kind) {
            case Kind::String: return str_val == other.str_val;
            case Kind::Int64: return i64_val == other.i64_val;
            case Kind::Uint64: return u64_val == other.u64_val;
            case Kind::Float64: return f64_val == other.f64_val;
            case Kind::Bool: return bool_val == other.bool_val;
        }
        return false;
    }

    std::string dump() const;
};

struct JsonValue {
    enum class Kind { Null, Bool, Int64, Uint64, Float64, String, Array, Object } kind{Kind::Null};
    bool bool_val{false};
    int64_t i64_val{0};
    uint64_t u64_val{0};
    double f64_val{0.0};
    std::string str_val;
    std::vector<JsonValue> array_val;
    std::map<std::string, JsonValue, std::less<>> object_val;

    static JsonValue null() noexcept { return JsonValue{}; }
    static JsonValue from_bool(bool b) noexcept {
        JsonValue v;
        v.kind = Kind::Bool;
        v.bool_val = b;
        return v;
    }
    static JsonValue from_int64(int64_t i) noexcept {
        JsonValue v;
        v.kind = Kind::Int64;
        v.i64_val = i;
        return v;
    }
    static JsonValue from_uint64(uint64_t u) noexcept {
        JsonValue v;
        v.kind = Kind::Uint64;
        v.u64_val = u;
        return v;
    }
    static JsonValue from_float64(double d) {
        if (!std::isfinite(d)) {
            throw std::invalid_argument("kale::openapi: JSON float value must be finite");
        }
        JsonValue v;
        v.kind = Kind::Float64;
        v.f64_val = d;
        return v;
    }
    static JsonValue from_string(std::string s) {
        JsonValue v;
        v.kind = Kind::String;
        v.str_val = std::move(s);
        return v;
    }
    static JsonValue from_array(std::vector<JsonValue> arr) {
        JsonValue v;
        v.kind = Kind::Array;
        v.array_val = std::move(arr);
        return v;
    }
    static JsonValue from_object(std::map<std::string, JsonValue, std::less<>> obj) {
        JsonValue v;
        v.kind = Kind::Object;
        v.object_val = std::move(obj);
        return v;
    }

    std::string dump() const;
};

struct OpenApiSchema {
    std::string type{"object"};
    std::string format;
    std::string description;
    bool nullable{false};

    std::optional<size_t> min_items;
    std::optional<size_t> max_items;

    std::optional<NumericBound> minimum;
    std::optional<NumericBound> maximum;

    std::vector<EnumValue> enum_values;

    std::unique_ptr<OpenApiSchema> items;
    std::map<std::string, OpenApiSchema, std::less<>> properties;
    std::vector<std::string> required;
    std::map<std::string, JsonValue, std::less<>> extensions;

    OpenApiSchema();
    ~OpenApiSchema();
    OpenApiSchema(const OpenApiSchema &other);
    OpenApiSchema &operator=(const OpenApiSchema &other);
    OpenApiSchema(OpenApiSchema &&other) noexcept;
    OpenApiSchema &operator=(OpenApiSchema &&other) noexcept;

    std::string dump(int indent = 0) const;
};

namespace detail {

template <typename T>
NumericBound make_numeric_bound(T val, bool exclusive) {
    NumericBound b;
    b.exclusive = exclusive;
    if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>) {
            b.kind = NumericBound::Kind::Int64;
            b.i64 = static_cast<int64_t>(val);
        } else {
            b.kind = NumericBound::Kind::Uint64;
            b.u64 = static_cast<uint64_t>(val);
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(val)) {
            throw std::invalid_argument("kale::openapi: numeric boundary must be finite");
        }
        b.kind = NumericBound::Kind::Float64;
        b.f64 = static_cast<double>(val);
    }
    return b;
}

inline void intersect_minimum(std::optional<NumericBound> &existing, const NumericBound &nb) {
    if (!existing.has_value()) {
        existing = nb;
        return;
    }
    double cur_val = existing->as_double();
    double new_val = nb.as_double();
    if (new_val > cur_val) {
        existing = nb;
    } else if (new_val == cur_val) {
        existing->exclusive = existing->exclusive || nb.exclusive;
    }
}

inline void intersect_maximum(std::optional<NumericBound> &existing, const NumericBound &nb) {
    if (!existing.has_value()) {
        existing = nb;
        return;
    }
    double cur_val = existing->as_double();
    double new_val = nb.as_double();
    if (new_val < cur_val) {
        existing = nb;
    } else if (new_val == cur_val) {
        existing->exclusive = existing->exclusive || nb.exclusive;
    }
}

inline void check_boundary_consistency(const std::optional<NumericBound> &min_b,
                                      const std::optional<NumericBound> &max_b,
                                      std::string_view field_name) {
    if (min_b.has_value() && max_b.has_value()) {
        double mn = min_b->as_double();
        double mx = max_b->as_double();
        if (mn > mx || (mn == mx && (min_b->exclusive || max_b->exclusive))) {
            throw std::invalid_argument("kale::openapi: contradictory numeric bounds for field '" +
                                       std::string(field_name) + "'");
        }
    }
}

template <typename T>
EnumValue make_enum_value(const T &val) {
    if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> || std::is_same_v<T, const char*>) {
        return EnumValue::from_string(std::string(val));
    } else if constexpr (std::is_same_v<T, bool>) {
        return EnumValue::from_bool(val);
    } else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>) {
            return EnumValue::from_int64(static_cast<int64_t>(val));
        } else {
            return EnumValue::from_uint64(static_cast<uint64_t>(val));
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        return EnumValue::from_float64(static_cast<double>(val));
    } else {
        static_assert(!sizeof(T), "Unsupported enum value type for OpenAPI schema reflection");
    }
}

template <typename DTO>
    requires HasKaleFields<DTO>
OpenApiSchema reflect_schema_impl(std::vector<const std::type_info*> &visited);

template <typename T>
OpenApiSchema reflect_type_schema(std::vector<const std::type_info*> &visited) {
    OpenApiSchema s;
    using Bare = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Bare, std::string> || std::is_same_v<Bare, std::string_view> || std::is_same_v<Bare, const char*>) {
        s.type = "string";
    } else if constexpr (std::is_same_v<Bare, bool>) {
        s.type = "boolean";
    } else if constexpr (std::is_integral_v<Bare>) {
        s.type = "integer";
        if constexpr (sizeof(Bare) <= 4) {
            if constexpr (std::is_unsigned_v<Bare>) {
                s.format = "uint32";
            } else {
                s.format = "int32";
            }
        } else {
            if constexpr (std::is_unsigned_v<Bare>) {
                s.format = "uint64";
            } else {
                s.format = "int64";
            }
        }
    } else if constexpr (std::is_floating_point_v<Bare>) {
        s.type = "number";
        if constexpr (std::is_same_v<Bare, float>) {
            s.format = "float";
        } else {
            s.format = "double";
        }
    } else if constexpr (is_vector_v<Bare>) {
        s.type = "array";
        using Elem = typename is_vector<Bare>::value_type;
        s.items = std::make_unique<OpenApiSchema>(reflect_type_schema<Elem>(visited));
    } else if constexpr (HasKaleFields<Bare>) {
        s = reflect_schema_impl<Bare>(visited);
    } else {
        s.type = "string";
    }
    return s;
}

template <typename InnerType, typename Rule>
void apply_rule_to_schema(OpenApiSchema &field_schema, bool &is_field_required, const std::string &field_name, const Rule &rule) {
    using R = std::decay_t<Rule>;
    if constexpr (std::is_same_v<R, rules::RequiredRule>) {
        field_schema.nullable = false;
        is_field_required = true;
    } else if constexpr (std::is_same_v<R, rules::MinLenRule>) {
        if constexpr (std::is_same_v<InnerType, std::string> || std::is_same_v<InnerType, std::string_view> || std::is_same_v<InnerType, const char*>) {
            field_schema.extensions["x-kale-min-byte-length"] = JsonValue::from_uint64(rule.min_size);
        } else if constexpr (is_vector_v<InnerType>) {
            if (!field_schema.min_items.has_value() || rule.min_size > *field_schema.min_items) {
                field_schema.min_items = rule.min_size;
            }
        }
    } else if constexpr (std::is_same_v<R, rules::MaxLenRule>) {
        if constexpr (std::is_same_v<InnerType, std::string> || std::is_same_v<InnerType, std::string_view> || std::is_same_v<InnerType, const char*>) {
            field_schema.extensions["x-kale-max-byte-length"] = JsonValue::from_uint64(rule.max_size);
        } else if constexpr (is_vector_v<InnerType>) {
            if (!field_schema.max_items.has_value() || rule.max_size < *field_schema.max_items) {
                field_schema.max_items = rule.max_size;
            }
        }
    } else if constexpr (std::is_same_v<R, rules::LenRangeRule>) {
        if constexpr (std::is_same_v<InnerType, std::string> || std::is_same_v<InnerType, std::string_view> || std::is_same_v<InnerType, const char*>) {
            field_schema.extensions["x-kale-min-byte-length"] = JsonValue::from_uint64(rule.min_size);
            field_schema.extensions["x-kale-max-byte-length"] = JsonValue::from_uint64(rule.max_size);
        } else if constexpr (is_vector_v<InnerType>) {
            field_schema.min_items = rule.min_size;
            field_schema.max_items = rule.max_size;
        }
    } else if constexpr (requires { R::name; } && R::name == "gt") {
        intersect_minimum(field_schema.minimum, make_numeric_bound(rule.boundary, true));
    } else if constexpr (requires { R::name; } && R::name == "gte") {
        intersect_minimum(field_schema.minimum, make_numeric_bound(rule.boundary, false));
    } else if constexpr (requires { R::name; } && R::name == "lt") {
        intersect_maximum(field_schema.maximum, make_numeric_bound(rule.boundary, true));
    } else if constexpr (requires { R::name; } && R::name == "lte") {
        intersect_maximum(field_schema.maximum, make_numeric_bound(rule.boundary, false));
    } else if constexpr (requires { R::name; } && R::name == "range") {
        intersect_minimum(field_schema.minimum, make_numeric_bound(rule.min_v, false));
        intersect_maximum(field_schema.maximum, make_numeric_bound(rule.max_v, false));
    } else if constexpr (std::is_same_v<R, rules::PatternRule>) {
        auto it = field_schema.extensions.find("x-kale-patterns");
        if (it == field_schema.extensions.end()) {
            field_schema.extensions["x-kale-patterns"] = JsonValue::from_array({JsonValue::from_string(rule.pattern())});
        } else {
            it->second.array_val.push_back(JsonValue::from_string(rule.pattern()));
        }
    } else if constexpr (requires { R::name; } && R::name == "one_of") {
        std::vector<EnumValue> choices;
        for (const auto &item : rule.choices) {
            choices.push_back(make_enum_value(item));
        }
        if (field_schema.enum_values.empty()) {
            field_schema.enum_values = std::move(choices);
        } else {
            std::vector<EnumValue> intersected;
            for (const auto &old_item : field_schema.enum_values) {
                if (std::find(choices.begin(), choices.end(), old_item) != choices.end()) {
                    intersected.push_back(old_item);
                }
            }
            if (intersected.empty()) {
                throw std::invalid_argument("kale::openapi: contradictory one_of constraints result in empty choice set for field '" + field_name + "'");
            }
            field_schema.enum_values = std::move(intersected);
        }
    } else if constexpr (std::is_same_v<R, rules::IsEmailRule>) {
        field_schema.format = "email";
        field_schema.extensions["x-kale-format"] = JsonValue::from_string("email");
    } else if constexpr (std::is_same_v<R, rules::IsUuidV7Rule>) {
        field_schema.format = "uuid";
        field_schema.extensions["x-kale-uuid-version"] = JsonValue::from_int64(7);
    } else if constexpr (std::is_same_v<R, rules::IsNumericRule>) {
        field_schema.extensions["x-kale-rule"] = JsonValue::from_string("is_numeric");
    } else if constexpr (std::is_same_v<R, rules::IsAlphanumericRule>) {
        field_schema.extensions["x-kale-rule"] = JsonValue::from_string("is_alphanumeric");
    } else if constexpr (std::is_same_v<R, rules::IsHexRule>) {
        field_schema.extensions["x-kale-rule"] = JsonValue::from_string("is_hex");
    } else if constexpr (std::is_same_v<R, rules::StartsWithRule>) {
        field_schema.extensions["x-kale-starts-with"] = JsonValue::from_string(rule.prefix);
    } else if constexpr (std::is_same_v<R, rules::EndsWithRule>) {
        field_schema.extensions["x-kale-ends-with"] = JsonValue::from_string(rule.suffix);
    } else if constexpr (std::is_same_v<R, rules::ContainsRule>) {
        field_schema.extensions["x-kale-contains"] = JsonValue::from_string(rule.needle);
    } else {
        field_schema.extensions["x-kale-custom-validation"] = JsonValue::from_bool(true);
    }
}

template <typename Field>
void reflect_field(OpenApiSchema &schema, const Field &field, std::vector<const std::type_info*> &visited) {
    using MemberType = typename std::decay_t<Field>::member_type;
    std::string key = std::string(field.key);

    if constexpr (is_optional_v<MemberType>) {
        using InnerType = typename is_optional<MemberType>::value_type;
        OpenApiSchema field_schema = reflect_type_schema<InnerType>(visited);
        field_schema.nullable = true;
        bool is_field_required = false;

        std::apply([&](const auto &...rule) {
            (apply_rule_to_schema<InnerType>(field_schema, is_field_required, key, rule), ...);
        }, field.constraints());

        check_boundary_consistency(field_schema.minimum, field_schema.maximum, key);
        schema.properties[key] = std::move(field_schema);
        if (is_field_required) {
            schema.required.push_back(key);
        }
    } else {
        using InnerType = MemberType;
        OpenApiSchema field_schema = reflect_type_schema<InnerType>(visited);
        field_schema.nullable = false;
        bool is_field_required = true;

        std::apply([&](const auto &...rule) {
            (apply_rule_to_schema<InnerType>(field_schema, is_field_required, key, rule), ...);
        }, field.constraints());

        check_boundary_consistency(field_schema.minimum, field_schema.maximum, key);
        schema.properties[key] = std::move(field_schema);
        if (is_field_required) {
            schema.required.push_back(key);
        }
    }
}

template <typename DTO>
    requires HasKaleFields<DTO>
OpenApiSchema reflect_schema_impl(std::vector<const std::type_info*> &visited) {
    const std::type_info *ti = &typeid(DTO);
    if (std::find(visited.begin(), visited.end(), ti) != visited.end()) {
        throw std::invalid_argument("kale::openapi: recursive DTO reflection detected for type: " + std::string(ti->name()));
    }
    visited.push_back(ti);
    OpenApiSchema schema;
    schema.type = "object";
    DTO dummy{};
    const auto &fields = dummy.__kale_fields();
    std::apply([&](const auto &...f) {
        (detail::reflect_field(schema, f, visited), ...);
    }, fields);
    visited.pop_back();
    return schema;
}

} // namespace detail

template <typename DTO>
    requires HasKaleFields<DTO>
inline OpenApiSchema reflect_schema() {
    std::vector<const std::type_info*> visited;
    return detail::reflect_schema_impl<DTO>(visited);
}

struct RouteDoc {
    std::string summary;
    std::string description;
    std::string operation_id;
    std::vector<std::string> tags;

    std::optional<OpenApiSchema> request_body;
    std::string request_body_description;
    std::string request_body_content_type{"application/json"};
    bool request_body_required{true};

    std::optional<OpenApiSchema> query_params;
    std::map<int, std::pair<std::string, std::optional<OpenApiSchema>>> responses;

    RouteDoc &set_summary(std::string s) { summary = std::move(s); return *this; }
    RouteDoc &set_description(std::string d) { description = std::move(d); return *this; }
    RouteDoc &set_operation_id(std::string id) { operation_id = std::move(id); return *this; }
    RouteDoc &add_tag(std::string t) { tags.push_back(std::move(t)); return *this; }

    template <typename DTO>
        requires HasKaleFields<DTO>
    RouteDoc &body(std::string desc = "", std::string content_type = "application/json") {
        request_body = reflect_schema<DTO>();
        request_body_description = std::move(desc);
        request_body_content_type = std::move(content_type);
        request_body_required = true;
        return *this;
    }

    template <typename DTO>
        requires HasKaleFields<DTO>
    RouteDoc &query() {
        query_params = reflect_schema<DTO>();
        return *this;
    }

    template <typename DTO>
        requires HasKaleFields<DTO>
    RouteDoc &response(int status, std::string desc = "") {
        responses[status] = {std::move(desc), reflect_schema<DTO>()};
        return *this;
    }

    RouteDoc &response(int status, std::string desc) {
        responses[status] = {std::move(desc), std::nullopt};
        return *this;
    }
};

class OpenApiRegistry {
public:
    RouteDoc &associate(HttpMethod method, std::string pattern, RouteDoc doc = {}) {
        auto key = std::make_pair(method, std::move(pattern));
        auto [it, inserted] = docs_.emplace(std::move(key), std::move(doc));
        if (!inserted) {
            throw std::invalid_argument("Duplicate RouteDoc registered for " +
                                       std::string(method_to_string(it->first.first)) + " " +
                                       it->first.second);
        }
        return it->second;
    }

    template <typename DTO>
        requires HasKaleFields<DTO>
    RouteDoc &associate_body(HttpMethod method, std::string pattern, std::string desc = "") {
        auto &doc = associate(method, std::move(pattern));
        doc.template body<DTO>(std::move(desc));
        return doc;
    }

    const std::map<std::pair<HttpMethod, std::string>, RouteDoc> &docs() const noexcept {
        return docs_;
    }

private:
    std::map<std::pair<HttpMethod, std::string>, RouteDoc> docs_;
};

class OpenApiDocument {
public:
    explicit OpenApiDocument(std::string json_doc) : json_doc_(std::move(json_doc)) {}

    std::string_view spec_json() const noexcept { return json_doc_; }
    const std::string &dump() const noexcept { return json_doc_; }

private:
    std::string json_doc_;
};

OpenApiDocument generate_openapi(
    const OpenApiConfig &config,
    const std::vector<RouteInfo> &routes,
    const OpenApiRegistry &registry = {}
);

Handler make_openapi_handler(OpenApiDocument doc);

} // namespace kale

#endif // KALE_OPENAPI_H
