#include <kale/http/openapi.h>

#include <charconv>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace kale {

namespace {

std::string escape_json_string(std::string_view s) {
    std::string res;
    res.reserve(s.size() + 2);
    res.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': res += "\\\""; break;
            case '\\': res += "\\\\"; break;
            case '\b': res += "\\b"; break;
            case '\f': res += "\\f"; break;
            case '\n': res += "\\n"; break;
            case '\r': res += "\\r"; break;
            case '\t': res += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    res += buf;
                } else {
                    res.push_back(c);
                }
                break;
        }
    }
    res.push_back('"');
    return res;
}

std::string to_lower_method(HttpMethod method) {
    std::string_view m = method_to_string(method);
    std::string res;
    res.reserve(m.size());
    for (char c : m) {
        res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return res;
}

std::string sanitize_identifier(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            res.push_back(c);
        } else if (c == '/' || c == '-' || c == '_' || c == '.' || c == ':') {
            if (!res.empty() && res.back() != '_') {
                res.push_back('_');
            }
        }
    }
    while (!res.empty() && res.back() == '_') res.pop_back();
    while (!res.empty() && res.front() == '_') res.erase(res.begin());
    return res.empty() ? "endpoint" : res;
}

struct ParsedParam {
    std::string name;
    std::string in{"path"};
    bool required{true};
    std::string description;
    OpenApiSchema schema;
};

struct NormalizedRoutePath {
    std::string openapi_path;
    std::string canonical_template;
    std::vector<ParsedParam> params;
};

NormalizedRoutePath normalize_openapi_path(std::string_view pattern) {
    NormalizedRoutePath result;
    std::vector<std::string_view> segments;

    size_t i = 0;
    while (i < pattern.size()) {
        size_t next_slash = pattern.find('/', i);
        if (next_slash == std::string_view::npos) {
            segments.push_back(pattern.substr(i));
            break;
        }
        segments.push_back(pattern.substr(i, next_slash - i));
        i = next_slash + 1;
    }
    if (!pattern.empty() && pattern.back() == '/') {
        segments.push_back("");
    }

    std::set<std::string> seen_param_names;
    std::string openapi_res;
    std::string canonical_res;

    for (size_t idx = 0; idx < segments.size(); ++idx) {
        if (idx > 0) {
            openapi_res.push_back('/');
            canonical_res.push_back('/');
        }
        std::string_view seg = segments[idx];
        if (seg.empty()) {
            continue;
        }

        if (seg.front() == ':') {
            std::string param_name(seg.substr(1));
            if (param_name.empty()) {
                throw std::invalid_argument("kale::openapi: empty path parameter name in route: " + std::string(pattern));
            }
            if (seen_param_names.contains(param_name)) {
                throw std::invalid_argument("kale::openapi: duplicate path parameter '" + param_name + "' in route: " + std::string(pattern));
            }
            seen_param_names.insert(param_name);

            openapi_res.append("{" + param_name + "}");
            canonical_res.append("{}");

            ParsedParam p;
            p.name = param_name;
            p.in = "path";
            p.required = true;
            p.schema.type = "string";
            result.params.push_back(std::move(p));
        } else if (seg.front() == '*') {
            std::string param_name(seg.substr(1));
            if (param_name.empty()) {
                param_name = "wildcard";
            }
            if (seen_param_names.contains(param_name)) {
                throw std::invalid_argument("kale::openapi: duplicate path parameter '" + param_name + "' in route: " + std::string(pattern));
            }
            seen_param_names.insert(param_name);

            openapi_res.append("{" + param_name + "}");
            canonical_res.append("{}");

            ParsedParam p;
            p.name = param_name;
            p.in = "path";
            p.required = true;
            p.description = "Catch-all wildcard path segment";
            p.schema.type = "string";
            result.params.push_back(std::move(p));
        } else {
            if (seg.find('{') != std::string_view::npos || seg.find('}') != std::string_view::npos) {
                throw std::invalid_argument("kale::openapi: unescaped literal braces in route segment '" + std::string(seg) + "'");
            }
            openapi_res.append(seg);
            canonical_res.append(seg);
        }
    }

    result.openapi_path = std::move(openapi_res);
    result.canonical_template = std::move(canonical_res);
    return result;
}

} // namespace

std::string NumericBound::dump() const {
    char buf[64];
    switch (kind) {
        case Kind::Int64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), i64);
            return std::string(buf, ptr - buf);
        }
        case Kind::Uint64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), u64);
            return std::string(buf, ptr - buf);
        }
        case Kind::Float64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), f64);
            if (ec == std::errc()) {
                return std::string(buf, ptr - buf);
            }
            std::snprintf(buf, sizeof(buf), "%.17g", f64);
            return std::string(buf);
        }
        default:
            return "0";
    }
}

std::string EnumValue::dump() const {
    char buf[64];
    switch (kind) {
        case Kind::String:
            return escape_json_string(str_val);
        case Kind::Int64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), i64_val);
            return std::string(buf, ptr - buf);
        }
        case Kind::Uint64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), u64_val);
            return std::string(buf, ptr - buf);
        }
        case Kind::Float64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), f64_val);
            if (ec == std::errc()) {
                return std::string(buf, ptr - buf);
            }
            std::snprintf(buf, sizeof(buf), "%.17g", f64_val);
            return std::string(buf);
        }
        case Kind::Bool:
            return bool_val ? "true" : "false";
    }
    return "null";
}

std::string JsonValue::dump() const {
    char buf[64];
    switch (kind) {
        case Kind::Null:
            return "null";
        case Kind::Bool:
            return bool_val ? "true" : "false";
        case Kind::Int64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), i64_val);
            return std::string(buf, ptr - buf);
        }
        case Kind::Uint64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), u64_val);
            return std::string(buf, ptr - buf);
        }
        case Kind::Float64: {
            auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), f64_val);
            if (ec == std::errc()) {
                return std::string(buf, ptr - buf);
            }
            std::snprintf(buf, sizeof(buf), "%.17g", f64_val);
            return std::string(buf);
        }
        case Kind::String:
            return escape_json_string(str_val);
        case Kind::Array: {
            std::string res = "[";
            for (size_t i = 0; i < array_val.size(); ++i) {
                if (i > 0) res.push_back(',');
                res.append(array_val[i].dump());
            }
            res.push_back(']');
            return res;
        }
        case Kind::Object: {
            std::string res = "{";
            bool first = true;
            for (const auto &[k, v] : object_val) {
                if (!first) res.push_back(',');
                first = false;
                res.append(escape_json_string(k));
                res.push_back(':');
                res.append(v.dump());
            }
            res.push_back('}');
            return res;
        }
    }
    return "null";
}

OpenApiSchema::OpenApiSchema() = default;
OpenApiSchema::~OpenApiSchema() = default;

OpenApiSchema::OpenApiSchema(const OpenApiSchema &other)
    : type(other.type),
      format(other.format),
      description(other.description),
      nullable(other.nullable),
      min_items(other.min_items),
      max_items(other.max_items),
      minimum(other.minimum),
      maximum(other.maximum),
      enum_values(other.enum_values),
      properties(other.properties),
      required(other.required),
      extensions(other.extensions) {
    if (other.items) {
        items = std::make_unique<OpenApiSchema>(*other.items);
    }
}

OpenApiSchema &OpenApiSchema::operator=(const OpenApiSchema &other) {
    if (this != &other) {
        type = other.type;
        format = other.format;
        description = other.description;
        nullable = other.nullable;
        min_items = other.min_items;
        max_items = other.max_items;
        minimum = other.minimum;
        maximum = other.maximum;
        enum_values = other.enum_values;
        properties = other.properties;
        required = other.required;
        extensions = other.extensions;
        if (other.items) {
            items = std::make_unique<OpenApiSchema>(*other.items);
        } else {
            items.reset();
        }
    }
    return *this;
}

OpenApiSchema::OpenApiSchema(OpenApiSchema &&other) noexcept = default;
OpenApiSchema &OpenApiSchema::operator=(OpenApiSchema &&other) noexcept = default;

std::string OpenApiSchema::dump(int /*indent*/) const {
    std::string res = "{";
    bool first = true;

    auto append_field = [&](std::string_view key, std::string_view val) {
        if (!first) res.push_back(',');
        first = false;
        res.append(escape_json_string(key));
        res.push_back(':');
        res.append(val);
    };

    if (!type.empty()) {
        append_field("type", escape_json_string(type));
    }
    if (!format.empty()) {
        append_field("format", escape_json_string(format));
    }
    if (!description.empty()) {
        append_field("description", escape_json_string(description));
    }
    if (nullable) {
        append_field("nullable", "true");
    }
    if (min_items.has_value()) {
        append_field("minItems", std::to_string(*min_items));
    }
    if (max_items.has_value()) {
        append_field("maxItems", std::to_string(*max_items));
    }
    if (minimum.has_value()) {
        append_field("minimum", minimum->dump());
        if (minimum->exclusive) {
            append_field("exclusiveMinimum", "true");
        }
    }
    if (maximum.has_value()) {
        append_field("maximum", maximum->dump());
        if (maximum->exclusive) {
            append_field("exclusiveMaximum", "true");
        }
    }
    if (!enum_values.empty()) {
        std::string arr = "[";
        for (size_t i = 0; i < enum_values.size(); ++i) {
            if (i > 0) arr.push_back(',');
            arr.append(enum_values[i].dump());
        }
        arr.push_back(']');
        append_field("enum", arr);
    }
    if (items) {
        append_field("items", items->dump());
    }
    if (!properties.empty()) {
        std::string props = "{";
        bool prop_first = true;
        for (const auto &[prop_name, prop_s] : properties) {
            if (!prop_first) props.push_back(',');
            prop_first = false;
            props.append(escape_json_string(prop_name));
            props.push_back(':');
            props.append(prop_s.dump());
        }
        props.push_back('}');
        append_field("properties", props);
    }
    if (!required.empty()) {
        std::string req = "[";
        for (size_t i = 0; i < required.size(); ++i) {
            if (i > 0) req.push_back(',');
            req.append(escape_json_string(required[i]));
        }
        req.push_back(']');
        append_field("required", req);
    }
    for (const auto &[ext_key, ext_val] : extensions) {
        append_field(ext_key, ext_val.dump());
    }

    res.push_back('}');
    return res;
}

OpenApiDocument generate_openapi(
    const OpenApiConfig &config,
    const std::vector<RouteInfo> &routes,
    const OpenApiRegistry &registry) {

    for (const auto &r : routes) {
        if (r.method == HttpMethod::CONNECT) {
            throw std::invalid_argument("kale::openapi: OpenAPI 3.0.3 does not support HTTP CONNECT method on route: " + r.pattern);
        }
    }

    // Orphan RouteDoc detection: every doc must match an actual route
    for (const auto &entry : registry.docs()) {
        const auto &[method, pattern] = entry.first;
        bool found = false;
        for (const auto &r : routes) {
            if (r.method == method && r.pattern == pattern) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::invalid_argument("kale::openapi: RouteDoc registered for " +
                                       std::string(method_to_string(method)) + " " +
                                       pattern + " but no matching route found in Router snapshot");
        }
    }

    // Path template collision detection
    std::map<std::string, std::string> canonical_templates;
    for (const auto &r : routes) {
        auto norm = normalize_openapi_path(r.pattern);
        auto [it, inserted] = canonical_templates.emplace(norm.canonical_template, r.pattern);
        if (!inserted && it->second != r.pattern) {
            throw std::invalid_argument("kale::openapi: path template collision between '" +
                                       it->second + "' and '" + r.pattern +
                                       "' (both resolve to '" + norm.canonical_template + "')");
        }
    }

    struct GeneratedOp {
        std::string operation_json;
    };
    std::map<std::string, std::map<std::string, GeneratedOp>> paths_map;
    std::set<std::string> seen_operation_ids;

    for (const auto &r : routes) {
        auto doc_it = registry.docs().find({r.method, r.pattern});
        if (doc_it == registry.docs().end()) {
            if (config.undocumented_policy == UndocumentedRoutePolicy::RequireExplicit) {
                throw std::invalid_argument("kale::openapi: route " + std::string(method_to_string(r.method)) +
                                           " " + r.pattern + " has no RouteDoc registered");
            } else if (config.undocumented_policy == UndocumentedRoutePolicy::Exclude) {
                continue;
            }
        }

        RouteDoc default_doc;
        const RouteDoc &doc = (doc_it != registry.docs().end()) ? doc_it->second : default_doc;

        auto norm = normalize_openapi_path(r.pattern);
        std::string method_lower = to_lower_method(r.method);

        if (paths_map[norm.openapi_path].contains(method_lower)) {
            throw std::invalid_argument("kale::openapi: duplicate operation registered for " +
                                       method_lower + " " + norm.openapi_path);
        }

        std::string op_id;
        if (!doc.operation_id.empty()) {
            op_id = doc.operation_id;
        } else if (!r.name.empty()) {
            op_id = r.name;
        } else {
            op_id = method_lower + "_" + sanitize_identifier(r.pattern);
        }
        if (seen_operation_ids.contains(op_id)) {
            throw std::invalid_argument("kale::openapi: duplicate operationId '" + op_id + "' in OpenAPI document");
        }
        seen_operation_ids.insert(op_id);

        std::string op_json = "{";
        bool op_first = true;
        auto append_op_field = [&](std::string_view k, std::string_view v) {
            if (!op_first) op_json.push_back(',');
            op_first = false;
            op_json.append(escape_json_string(k));
            op_json.push_back(':');
            op_json.append(v);
        };

        append_op_field("operationId", escape_json_string(op_id));

        std::string summary = !doc.summary.empty() ? doc.summary : (std::string(method_to_string(r.method)) + " " + r.pattern);
        append_op_field("summary", escape_json_string(summary));

        if (!doc.description.empty()) {
            append_op_field("description", escape_json_string(doc.description));
        }

        std::vector<std::string> combined_tags = doc.tags;
        if (combined_tags.empty()) {
            auto tag_it = r.tags.find("tag");
            if (tag_it != r.tags.end() && !tag_it->second.empty()) {
                combined_tags.push_back(tag_it->second);
            }
        }
        if (!combined_tags.empty()) {
            std::string tags_arr = "[";
            for (size_t t = 0; t < combined_tags.size(); ++t) {
                if (t > 0) tags_arr.push_back(',');
                tags_arr.append(escape_json_string(combined_tags[t]));
            }
            tags_arr.push_back(']');
            append_op_field("tags", tags_arr);
        }

        if (!r.tags.empty()) {
            std::map<std::string, JsonValue, std::less<>> ext_tags;
            for (const auto &[k, v] : r.tags) {
                ext_tags[k] = JsonValue::from_string(v);
            }
            append_op_field("x-kale-route-tags", JsonValue::from_object(std::move(ext_tags)).dump());
        }

        std::vector<std::string> param_objects;
        for (const auto &p : norm.params) {
            std::string p_json = "{";
            p_json.append("\"name\":").append(escape_json_string(p.name));
            p_json.append(",\"in\":\"path\"");
            p_json.append(",\"required\":true");
            if (!p.description.empty()) {
                p_json.append(",\"description\":").append(escape_json_string(p.description));
            }
            p_json.append(",\"schema\":").append(p.schema.dump());
            p_json.push_back('}');
            param_objects.push_back(std::move(p_json));
        }

        if (doc.query_params.has_value()) {
            for (const auto &[q_name, q_schema] : doc.query_params->properties) {
                if (q_schema.type == "object") {
                    throw std::invalid_argument("kale::openapi: query parameter '" + q_name +
                                               "' does not support nested object types");
                }
                std::string q_json = "{";
                q_json.append("\"name\":").append(escape_json_string(q_name));
                q_json.append(",\"in\":\"query\"");
                bool req = std::find(doc.query_params->required.begin(), doc.query_params->required.end(), q_name) != doc.query_params->required.end();
                q_json.append(",\"required\":").append(req ? "true" : "false");
                if (!q_schema.description.empty()) {
                    q_json.append(",\"description\":").append(escape_json_string(q_schema.description));
                }
                if (q_schema.type == "array") {
                    q_json.append(",\"style\":\"form\",\"explode\":true");
                }
                q_json.append(",\"schema\":").append(q_schema.dump());
                q_json.push_back('}');
                param_objects.push_back(std::move(q_json));
            }
        }

        if (!param_objects.empty()) {
            std::string params_arr = "[";
            for (size_t p = 0; p < param_objects.size(); ++p) {
                if (p > 0) params_arr.push_back(',');
                params_arr.append(param_objects[p]);
            }
            params_arr.push_back(']');
            append_op_field("parameters", params_arr);
        }

        if (doc.request_body.has_value()) {
            std::string rb_json = "{";
            if (!doc.request_body_description.empty()) {
                rb_json.append("\"description\":").append(escape_json_string(doc.request_body_description)).append(",");
            }
            rb_json.append("\"required\":").append(doc.request_body_required ? "true" : "false").append(",");
            std::string content_type = doc.request_body_content_type.empty() ? "application/json" : doc.request_body_content_type;
            rb_json.append("\"content\":{");
            rb_json.append(escape_json_string(content_type)).append(":{\"schema\":").append(doc.request_body->dump()).append("}}");
            rb_json.push_back('}');
            append_op_field("requestBody", rb_json);
        }

        std::string resp_json = "{";
        if (!doc.responses.empty()) {
            bool resp_first = true;
            for (const auto &[status_code, resp_pair] : doc.responses) {
                if (!resp_first) resp_json.push_back(',');
                resp_first = false;
                const auto &[desc, schema_opt] = resp_pair;
                resp_json.append(escape_json_string(std::to_string(status_code)));
                resp_json.append(":{");
                std::string desc_text = !desc.empty() ? desc : ("Status " + std::to_string(status_code));
                resp_json.append("\"description\":").append(escape_json_string(desc_text));
                if (schema_opt.has_value()) {
                    resp_json.append(",\"content\":{\"application/json\":{\"schema\":");
                    resp_json.append(schema_opt->dump());
                    resp_json.append("}}");
                }
                resp_json.push_back('}');
            }
        } else {
            resp_json.append("\"200\":{\"description\":\"OK\"}");
        }
        resp_json.push_back('}');
        append_op_field("responses", resp_json);

        op_json.push_back('}');
        paths_map[norm.openapi_path][method_lower] = GeneratedOp{std::move(op_json)};
    }

    std::string root = "{";
    root.append("\"openapi\":\"3.0.3\",");
    root.append("\"info\":{");
    root.append("\"title\":").append(escape_json_string(config.title)).append(",");
    root.append("\"version\":").append(escape_json_string(config.version));
    if (!config.description.empty()) {
        root.append(",\"description\":").append(escape_json_string(config.description));
    }
    root.append("},");

    root.append("\"paths\":{");
    bool path_first = true;
    for (const auto &[path_str, methods] : paths_map) {
        if (!path_first) root.push_back(',');
        path_first = false;
        root.append(escape_json_string(path_str));
        root.append(":{");
        bool m_first = true;
        for (const auto &[m_str, op] : methods) {
            if (!m_first) root.push_back(',');
            m_first = false;
            root.append(escape_json_string(m_str));
            root.push_back(':');
            root.append(op.operation_json);
        }
        root.push_back('}');
    }
    root.append("}}");

    return OpenApiDocument(std::move(root));
}

Handler make_openapi_handler(OpenApiDocument doc) {
    return Handler::from_sync([doc = std::move(doc)](Context &ctx) {
        ctx.set_header("Content-Type", "application/json; charset=utf-8")
           .string(doc.dump());
    });
}

} // namespace kale
