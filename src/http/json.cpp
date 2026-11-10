#include "kale/http/json.h"

#include <cstring>
#include <cstdio>
#include <charconv>
#include <limits>

namespace kale {

namespace {
struct StackKey {
    constexpr static size_t STACK_MAX = 64;
    char buf[STACK_MAX];
    std::string heap;
    const char *c_str;

    explicit StackKey(std::string_view sv) {
        if (sv.size() < STACK_MAX) {
            if (!sv.empty()) std::memcpy(buf, sv.data(), sv.size());
            buf[sv.size()] = '\0';
            c_str = buf;
        } else {
            heap = std::string(sv);
            c_str = heap.c_str();
        }
    }
};

static const json_value_t *set_obj_null(json_object_t *obj, const char *key, const json_value_t *existing) {
    if (existing) {
        const json_value_t *inserted = json_object_insert_before(existing, obj, key, JSON_VALUE_NULL);
        json_value_destroy(json_object_remove(existing, obj));
        return inserted;
    }
    return json_object_append(obj, key, JSON_VALUE_NULL);
}

static const json_value_t *set_obj_bool(json_object_t *obj, const char *key, const json_value_t *existing, bool val) {
    int type = val ? JSON_VALUE_TRUE : JSON_VALUE_FALSE;
    if (existing) {
        const json_value_t *inserted = json_object_insert_before(existing, obj, key, type);
        json_value_destroy(json_object_remove(existing, obj));
        return inserted;
    }
    return json_object_append(obj, key, type);
}

static const json_value_t *set_obj_number(json_object_t *obj, const char *key, const json_value_t *existing, double val) {
    if (existing) {
        const json_value_t *inserted = json_object_insert_before(existing, obj, key, JSON_VALUE_NUMBER, val);
        json_value_destroy(json_object_remove(existing, obj));
        return inserted;
    }
    return json_object_append(obj, key, JSON_VALUE_NUMBER, val);
}

static const json_value_t *set_obj_string(json_object_t *obj, const char *key, const json_value_t *existing, const char *val) {
    if (existing) {
        const json_value_t *inserted = json_object_insert_before(existing, obj, key, JSON_VALUE_STRING, val);
        json_value_destroy(json_object_remove(existing, obj));
        return inserted;
    }
    return json_object_append(obj, key, JSON_VALUE_STRING, val);
}

static const json_value_t *set_obj_move_node(json_object_t *obj, const char *key, const json_value_t *existing, json_value_t *node) {
    if (!node) {
        return set_obj_null(obj, key, existing);
    }
    if (existing) {
        const json_value_t *inserted = json_object_insert_before(existing, obj, key, 0, node);
        json_value_destroy(json_object_remove(existing, obj));
        return inserted;
    }
    return json_object_append(obj, key, 0, node);
}

static const json_value_t *set_arr_null(json_array_t *arr, const json_value_t *existing) {
    const json_value_t *inserted = json_array_insert_before(existing, arr, JSON_VALUE_NULL);
    json_value_destroy(json_array_remove(existing, arr));
    return inserted;
}

static const json_value_t *set_arr_bool(json_array_t *arr, const json_value_t *existing, bool val) {
    int type = val ? JSON_VALUE_TRUE : JSON_VALUE_FALSE;
    const json_value_t *inserted = json_array_insert_before(existing, arr, type);
    json_value_destroy(json_array_remove(existing, arr));
    return inserted;
}

static const json_value_t *set_arr_number(json_array_t *arr, const json_value_t *existing, double val) {
    const json_value_t *inserted = json_array_insert_before(existing, arr, JSON_VALUE_NUMBER, val);
    json_value_destroy(json_array_remove(existing, arr));
    return inserted;
}

static const json_value_t *set_arr_string(json_array_t *arr, const json_value_t *existing, const char *val) {
    const json_value_t *inserted = json_array_insert_before(existing, arr, JSON_VALUE_STRING, val);
    json_value_destroy(json_array_remove(existing, arr));
    return inserted;
}

static const json_value_t *set_arr_move_node(json_array_t *arr, const json_value_t *existing, json_value_t *node) {
    if (!node) {
        return set_arr_null(arr, existing);
    }
    const json_value_t *inserted = json_array_insert_before(existing, arr, 0, node);
    json_value_destroy(json_array_remove(existing, arr));
    return inserted;
}
} // namespace

Json::Json()
    : node_(json_value_create(JSON_VALUE_NULL)),
      parent_(nullptr),
      allocated_(true) {}

Json::Json(std::nullptr_t)
    : node_(json_value_create(JSON_VALUE_NULL)),
      parent_(nullptr),
      allocated_(true) {}

Json::Json(bool val)
    : node_(json_value_create(val ? JSON_VALUE_TRUE : JSON_VALUE_FALSE)),
      parent_(nullptr),
      allocated_(true) {}

Json::Json(std::string_view str)
    : parent_(nullptr), allocated_(true) {
    constexpr size_t STACK_STRING_SIZE = 256;
    if (str.size() < STACK_STRING_SIZE) {
        char stack_buf[STACK_STRING_SIZE];
        if (!str.empty()) std::memcpy(stack_buf, str.data(), str.size());
        stack_buf[str.size()] = '\0';
        node_ = json_value_create(JSON_VALUE_STRING, stack_buf);
    } else {
        std::string s(str);
        node_ = json_value_create(JSON_VALUE_STRING, s.c_str());
    }
}

Json::Json(const std::string &str)
    : node_(json_value_create(JSON_VALUE_STRING, str.c_str())),
      parent_(nullptr),
      allocated_(true) {}

Json::Json(const char *str)
    : node_(json_value_create(JSON_VALUE_STRING, str ? str : "")),
      parent_(nullptr),
      allocated_(true) {}

Json::Json(const std::vector<std::string> &val)
    : node_(json_value_create(JSON_VALUE_ARRAY)),
      parent_(nullptr),
      allocated_(true) {
    json_array_t *arr = json_value_array(node_);
    for (const auto &s : val) {
        json_array_append(arr, JSON_VALUE_STRING, s.c_str());
    }
}

Json::Json(JsonType type)
    : node_(json_value_create(type == JsonType::Object ? JSON_VALUE_OBJECT : JSON_VALUE_ARRAY)),
      parent_(nullptr),
      allocated_(true) {}

Json::Json(const json_value_t *node, const json_value_t *parent, std::string key)
    : node_(const_cast<json_value_t *>(node)),
      parent_(parent),
      allocated_(false),
      parent_key_(std::move(key)) {}

Json Json::parse(std::string_view str) {
    if (str.find('\0') != std::string_view::npos) throw JsonParseError("Invalid JSON");
    // Small string parsing uses a stack buffer to avoid heap allocation.
    constexpr size_t STACK_JSON_PARSE_SIZE = 4 * 1024;
    if (str.size() < STACK_JSON_PARSE_SIZE) {
        char stack_buf[STACK_JSON_PARSE_SIZE];
        std::memcpy(stack_buf, str.data(), str.size());
        stack_buf[str.size()] = '\0';
        json_value_t *parsed = json_value_parse(stack_buf);
        if (!parsed) throw JsonParseError("Invalid JSON");
        return Json(parsed, true);
    }
    std::string s(str);
    json_value_t *parsed = json_value_parse(s.c_str());
    if (!parsed) throw JsonParseError("Invalid JSON");
    return Json(parsed, true);
}

Json Json::parse(FILE *fp) {
    if (!fp) throw JsonParseError("Null JSON file");
    std::string content;
    char buffer[4096];
    while (size_t bytes = fread(buffer, 1, sizeof(buffer), fp)) {
        content.append(buffer, bytes);
    }
    if (ferror(fp)) throw JsonParseError("Cannot read JSON file");
    return parse(content);
}

void Json::destroy_node() noexcept {
    if (allocated_ && node_) {
        json_value_destroy(node_);
        node_ = nullptr;
        allocated_ = false;
    }
}

void Json::reset() noexcept {
    node_ = nullptr;
    parent_ = nullptr;
    allocated_ = false;
    parent_key_.clear();
}

Json::~Json() {
    destroy_node();
}

Json Json::copy() const {
    json_value_t *cp = node_ ? json_value_copy(node_) : nullptr;
    Json js(cp, cp != nullptr);
    js.parent_key_ = parent_key_;
    return js;
}

Json::Json(const Json &other)
    : node_(other.node_ ? json_value_copy(other.node_) : nullptr),
      parent_(nullptr),
      allocated_(node_ != nullptr),
      parent_key_(other.parent_key_) {}

Json &Json::operator=(const Json &other) {
    if (parent_ != nullptr) {
        assign_proxy(other);
        return *this;
    }
    if (this == &other) {
        return *this;
    }
    json_value_t *new_node = other.node_ ? json_value_copy(other.node_) : nullptr;
    destroy_node();
    node_ = new_node;
    allocated_ = (node_ != nullptr);
    parent_ = nullptr;
    parent_key_ = other.parent_key_;
    return *this;
}

Json::Json(Json &&other) noexcept
    : node_(other.node_),
      parent_(other.parent_),
      allocated_(other.allocated_),
      parent_key_(std::move(other.parent_key_)) {
    other.node_ = nullptr;
    other.parent_ = nullptr;
    other.allocated_ = false;
    other.parent_key_.clear();
}

Json &Json::operator=(Json &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (parent_ != nullptr) {
        assign_proxy(std::move(other));
        return *this;
    }
    if (other.allocated_ && other.parent_ == nullptr) {
        destroy_node();
        node_ = other.node_;
        parent_ = nullptr;
        allocated_ = true;
        parent_key_ = std::move(other.parent_key_);
        other.node_ = nullptr;
        other.parent_ = nullptr;
        other.allocated_ = false;
        other.parent_key_.clear();
    } else {
        json_value_t *new_node = other.node_ ? json_value_copy(other.node_) : nullptr;
        destroy_node();
        node_ = new_node;
        allocated_ = (node_ != nullptr);
        parent_ = nullptr;
        parent_key_ = std::move(other.parent_key_);
        other.node_ = nullptr;
        other.parent_ = nullptr;
        other.allocated_ = false;
        other.parent_key_.clear();
    }
    return *this;
}

Json &Json::operator=(std::nullptr_t) {
    assign_null();
    return *this;
}

Json &Json::operator=(bool value) {
    assign_bool(value);
    return *this;
}

Json &Json::operator=(const char *value) {
    assign_string(value);
    return *this;
}

Json &Json::operator=(std::string_view value) {
    assign_string_view(value);
    return *this;
}

Json &Json::operator=(const std::string &value) {
    assign_string(value.c_str());
    return *this;
}

void Json::assign_bool(bool value) {
    if (!parent_) {
        destroy_node();
        node_ = json_value_create(value ? JSON_VALUE_TRUE : JSON_VALUE_FALSE);
        allocated_ = true;
    } else {
        assign_proxy_bool(value);
    }
}

void Json::assign_number(double value) {
    if (!parent_) {
        destroy_node();
        node_ = json_value_create(JSON_VALUE_NUMBER, value);
        allocated_ = true;
    } else {
        assign_proxy_number(value);
    }
}

void Json::assign_null() {
    if (!parent_) {
        destroy_node();
        node_ = json_value_create(JSON_VALUE_NULL);
        allocated_ = true;
    } else {
        assign_proxy_null();
    }
}

void Json::assign_string(const char *value) {
    if (!parent_) {
        json_value_t *new_node = json_value_create(JSON_VALUE_STRING, value ? value : "");
        destroy_node();
        node_ = new_node;
        allocated_ = true;
    } else {
        assign_proxy_string(value ? value : "");
    }
}

void Json::assign_string_view(std::string_view value) {
    if (!parent_) {
        constexpr size_t STACK_STRING_SIZE = 256;
        json_value_t *new_node = nullptr;
        if (value.size() < STACK_STRING_SIZE) {
            char stack_buf[STACK_STRING_SIZE];
            if (!value.empty()) std::memcpy(stack_buf, value.data(), value.size());
            stack_buf[value.size()] = '\0';
            new_node = json_value_create(JSON_VALUE_STRING, stack_buf);
        } else {
            std::string s(value);
            new_node = json_value_create(JSON_VALUE_STRING, s.c_str());
        }
        destroy_node();
        node_ = new_node;
        allocated_ = true;
    } else {
        assign_proxy_string_view(value);
    }
}

int Json::type() const noexcept {
    return node_ ? json_value_type(node_) : 0;
}

std::string Json::type_str() const {
    if (!node_) return "invalid";
    switch (type()) {
        case JSON_VALUE_STRING: return "string";
        case JSON_VALUE_NUMBER: return "number";
        case JSON_VALUE_OBJECT: return "object";
        case JSON_VALUE_ARRAY:  return "array";
        case JSON_VALUE_TRUE:
        case JSON_VALUE_FALSE: return "boolean";
        case JSON_VALUE_NULL:  return "null";
        default:               return "invalid";
    }
}

size_t Json::size() const noexcept {
    if (is_object()) {
        return json_object_size(json_value_object(node_));
    }
    if (is_array()) {
        return json_array_size(json_value_array(node_));
    }
    return 0;
}

bool Json::empty() const noexcept {
    return size() == 0;
}

void Json::clear() {
    if (is_object()) {
        json_object_t *obj = json_value_object(node_);
        const json_value_t *first = nullptr;
        while ((first = json_object_next_value(nullptr, obj)) != nullptr) {
            json_value_destroy(json_object_remove(first, obj));
        }
    } else if (is_array()) {
        json_array_t *arr = json_value_array(node_);
        const json_value_t *first = nullptr;
        while ((first = json_array_next_value(nullptr, arr)) != nullptr) {
            json_value_destroy(json_array_remove(first, arr));
        }
    }
}

bool Json::has(std::string_view key) const noexcept {
    if (!is_object()) return false;
    StackKey sk(key);
    return json_object_find(sk.c_str, json_value_object(node_)) != nullptr;
}

bool Json::to_object() {
    if (!allocated_ || !is_null()) {
        return false;
    }
    destroy_node();
    node_ = json_value_create(JSON_VALUE_OBJECT);
    allocated_ = true;
    return true;
}

bool Json::to_array() {
    if (!allocated_ || !is_null()) {
        return false;
    }
    destroy_node();
    node_ = json_value_create(JSON_VALUE_ARRAY);
    allocated_ = true;
    return true;
}

bool Json::can_obj_push_back() {
    if (is_root() && is_null()) {
        to_object();
    }
    return is_object();
}

bool Json::can_arr_push_back() {
    if (is_root() && is_null()) {
        to_array();
    }
    return is_array();
}

void Json::assign_proxy_bool(bool val) {
    if (parent_ == nullptr) return;
    int pt = json_value_type(parent_);
    if (pt == JSON_VALUE_OBJECT) {
        json_object_t *obj = json_value_object(parent_);
        const json_value_t *existing = is_placeholder() ? nullptr : (node_ ? node_ : json_object_find(parent_key_.c_str(), obj));
        destroy_node();
        const json_value_t *res = set_obj_bool(obj, parent_key_.c_str(), existing, val);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else if (pt == JSON_VALUE_ARRAY) {
        if (!node_) return;
        json_array_t *arr = json_value_array(parent_);
        const json_value_t *res = set_arr_bool(arr, node_, val);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    }
}

void Json::assign_proxy_number(double val) {
    if (parent_ == nullptr) return;
    int pt = json_value_type(parent_);
    if (pt == JSON_VALUE_OBJECT) {
        json_object_t *obj = json_value_object(parent_);
        const json_value_t *existing = is_placeholder() ? nullptr : (node_ ? node_ : json_object_find(parent_key_.c_str(), obj));
        destroy_node();
        const json_value_t *res = set_obj_number(obj, parent_key_.c_str(), existing, val);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else if (pt == JSON_VALUE_ARRAY) {
        if (!node_) return;
        json_array_t *arr = json_value_array(parent_);
        const json_value_t *res = set_arr_number(arr, node_, val);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    }
}

void Json::assign_proxy_null() {
    if (parent_ == nullptr) return;
    int pt = json_value_type(parent_);
    if (pt == JSON_VALUE_OBJECT) {
        json_object_t *obj = json_value_object(parent_);
        const json_value_t *existing = is_placeholder() ? nullptr : (node_ ? node_ : json_object_find(parent_key_.c_str(), obj));
        destroy_node();
        const json_value_t *res = set_obj_null(obj, parent_key_.c_str(), existing);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else if (pt == JSON_VALUE_ARRAY) {
        if (!node_) return;
        json_array_t *arr = json_value_array(parent_);
        const json_value_t *res = set_arr_null(arr, node_);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    }
}

void Json::assign_proxy_string(const char *val) {
    if (parent_ == nullptr) return;
    int pt = json_value_type(parent_);
    const char *v = val ? val : "";
    if (pt == JSON_VALUE_OBJECT) {
        json_object_t *obj = json_value_object(parent_);
        const json_value_t *existing = is_placeholder() ? nullptr : (node_ ? node_ : json_object_find(parent_key_.c_str(), obj));
        destroy_node();
        const json_value_t *res = set_obj_string(obj, parent_key_.c_str(), existing, v);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else if (pt == JSON_VALUE_ARRAY) {
        if (!node_) return;
        json_array_t *arr = json_value_array(parent_);
        const json_value_t *res = set_arr_string(arr, node_, v);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    }
}

void Json::assign_proxy_string_view(std::string_view val) {
    constexpr size_t STACK_STRING_SIZE = 256;
    if (val.size() < STACK_STRING_SIZE) {
        char stack_buf[STACK_STRING_SIZE];
        if (!val.empty()) std::memcpy(stack_buf, val.data(), val.size());
        stack_buf[val.size()] = '\0';
        assign_proxy_string(stack_buf);
    } else {
        std::string s(val);
        assign_proxy_string(s.c_str());
    }
}

void Json::assign_proxy(const Json &val) {
    if (parent_ == nullptr) return;
    int pt = json_value_type(parent_);
    Json cp = val.copy();
    json_value_t *n = cp.node_;
    cp.reset();
    if (pt == JSON_VALUE_OBJECT) {
        json_object_t *obj = json_value_object(parent_);
        const json_value_t *existing = is_placeholder() ? nullptr : (node_ ? node_ : json_object_find(parent_key_.c_str(), obj));
        destroy_node();
        const json_value_t *res = set_obj_move_node(obj, parent_key_.c_str(), existing, n);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else if (pt == JSON_VALUE_ARRAY) {
        if (!node_) {
            if (n) json_value_destroy(n);
            return;
        }
        json_array_t *arr = json_value_array(parent_);
        const json_value_t *res = set_arr_move_node(arr, node_, n);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else {
        if (n) json_value_destroy(n);
    }
}

void Json::assign_proxy(Json &&val) {
    if (parent_ == nullptr) return;
    int pt = json_value_type(parent_);
    json_value_t *n = nullptr;
    if (val.allocated_ && val.node_) {
        n = val.node_;
        val.node_ = nullptr;
        val.allocated_ = false;
    } else {
        Json cp = val.copy();
        n = cp.node_;
        cp.reset();
    }
    if (pt == JSON_VALUE_OBJECT) {
        json_object_t *obj = json_value_object(parent_);
        const json_value_t *existing = is_placeholder() ? nullptr : (node_ ? node_ : json_object_find(parent_key_.c_str(), obj));
        destroy_node();
        const json_value_t *res = set_obj_move_node(obj, parent_key_.c_str(), existing, n);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else if (pt == JSON_VALUE_ARRAY) {
        if (!node_) {
            if (n) json_value_destroy(n);
            return;
        }
        json_array_t *arr = json_value_array(parent_);
        const json_value_t *res = set_arr_move_node(arr, node_, n);
        node_ = const_cast<json_value_t *>(res);
        allocated_ = false;
    } else {
        if (n) json_value_destroy(n);
    }
}

void Json::push_back_obj(std::string_view key, const Json &val) {
    push_back(key, val);
}

void Json::push_back_obj(std::string_view key, Json &&val) {
    push_back(key, std::move(val));
}

void Json::update_arr(const Json &val) {
    if (!parent_ || !node_) return;
    json_array_t *arr = json_value_array(parent_);
    Json cp = val.copy();
    json_value_t *n = cp.node_;
    cp.reset();
    const json_value_t *res = set_arr_move_node(arr, node_, n);
    node_ = const_cast<json_value_t *>(res);
    allocated_ = false;
}

Json &Json::push_back(std::string_view key, const Json &val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }
    if (!can_obj_push_back()) return *this;
    json_object_t *obj = json_value_object(node_);
    StackKey sk(key);
    const json_value_t *existing = json_object_find(sk.c_str, obj);
    Json cp = val.copy();
    json_value_t *n = cp.node_;
    cp.reset();
    set_obj_move_node(obj, sk.c_str, existing, n);
    return *this;
}

Json &Json::push_back(std::string_view key, Json &&val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }
    if (!can_obj_push_back()) return *this;
    json_object_t *obj = json_value_object(node_);
    StackKey sk(key);
    const json_value_t *existing = json_object_find(sk.c_str, obj);
    json_value_t *n = nullptr;
    if (val.allocated_ && val.node_) {
        n = val.node_;
        val.node_ = nullptr;
        val.allocated_ = false;
    } else {
        Json cp = val.copy();
        n = cp.node_;
        cp.reset();
    }
    set_obj_move_node(obj, sk.c_str, existing, n);
    return *this;
}

Json &Json::push_back(std::string_view key, std::nullptr_t) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }
    if (!can_obj_push_back()) return *this;
    json_object_t *obj = json_value_object(node_);
    StackKey sk(key);
    const json_value_t *existing = json_object_find(sk.c_str, obj);
    set_obj_null(obj, sk.c_str, existing);
    return *this;
}

Json &Json::push_back(std::string_view key, bool val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }
    if (!can_obj_push_back()) return *this;
    json_object_t *obj = json_value_object(node_);
    StackKey sk(key);
    const json_value_t *existing = json_object_find(sk.c_str, obj);
    set_obj_bool(obj, sk.c_str, existing, val);
    return *this;
}

void Json::push_back_obj_number(std::string_view key, double val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }
    if (!can_obj_push_back()) return;
    json_object_t *obj = json_value_object(node_);
    StackKey sk(key);
    const json_value_t *existing = json_object_find(sk.c_str, obj);
    set_obj_number(obj, sk.c_str, existing, val);
}

Json &Json::push_back(std::string_view key, const char *val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }
    if (!can_obj_push_back()) return *this;
    json_object_t *obj = json_value_object(node_);
    StackKey sk(key);
    const char *v = val ? val : "";
    const json_value_t *existing = json_object_find(sk.c_str, obj);
    set_obj_string(obj, sk.c_str, existing, v);
    return *this;
}

Json &Json::push_back(std::string_view key, std::string_view val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }
    if (!can_obj_push_back()) return *this;
    json_object_t *obj = json_value_object(node_);
    StackKey sk(key);
    StackKey sv(val);
    const json_value_t *existing = json_object_find(sk.c_str, obj);
    set_obj_string(obj, sk.c_str, existing, sv.c_str);
    return *this;
}

Json &Json::push_back(std::string_view key, const std::string &val) {
    return push_back(key, val.c_str());
}

Json &Json::push_back(const Json &val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_ARRAY));
        allocated_ = false;
    }
    if (!can_arr_push_back()) return *this;
    json_array_t *arr = json_value_array(node_);
    Json cp = val.copy();
    json_value_t *n = cp.node_;
    cp.reset();
    json_array_append(arr, 0, n);
    return *this;
}

Json &Json::push_back(Json &&val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_ARRAY));
        allocated_ = false;
    }
    if (!can_arr_push_back()) return *this;
    json_array_t *arr = json_value_array(node_);
    if (val.allocated_ && val.node_) {
        json_value_t *n = val.node_;
        val.node_ = nullptr;
        val.allocated_ = false;
        json_array_append(arr, 0, n);
    } else {
        Json cp = val.copy();
        json_value_t *n = cp.node_;
        cp.reset();
        json_array_append(arr, 0, n);
    }
    return *this;
}

Json &Json::push_back(std::nullptr_t) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_ARRAY));
        allocated_ = false;
    }
    if (!can_arr_push_back()) return *this;
    json_array_t *arr = json_value_array(node_);
    json_array_append(arr, JSON_VALUE_NULL);
    return *this;
}

Json &Json::push_back(bool val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_ARRAY));
        allocated_ = false;
    }
    if (!can_arr_push_back()) return *this;
    json_array_t *arr = json_value_array(node_);
    json_array_append(arr, val ? JSON_VALUE_TRUE : JSON_VALUE_FALSE);
    return *this;
}

void Json::push_back_arr_number(double val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_ARRAY));
        allocated_ = false;
    }
    if (!can_arr_push_back()) return;
    json_array_t *arr = json_value_array(node_);
    json_array_append(arr, JSON_VALUE_NUMBER, val);
}

Json &Json::push_back(const char *val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_ARRAY));
        allocated_ = false;
    }
    if (!can_arr_push_back()) return *this;
    json_array_t *arr = json_value_array(node_);
    json_array_append(arr, JSON_VALUE_STRING, val ? val : "");
    return *this;
}

Json &Json::push_back(std::string_view val) {
    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_ARRAY));
        allocated_ = false;
    }
    if (!can_arr_push_back()) return *this;
    json_array_t *arr = json_value_array(node_);
    StackKey sv(val);
    json_array_append(arr, JSON_VALUE_STRING, sv.c_str);
    return *this;
}

Json &Json::push_back(const std::string &val) {
    return push_back(val.c_str());
}

void Json::erase(std::string_view key) {
    if (!is_object()) return;
    StackKey sk(key);
    json_object_t *obj = json_value_object(node_);
    const json_value_t *find = json_object_find(sk.c_str, obj);
    if (!find) return;
    json_value_t *rem = json_object_remove(find, obj);
    json_value_destroy(rem);
}

void Json::erase(int index) {
    if (!is_array() || index < 0 || index >= static_cast<int>(size())) return;
    json_array_t *arr = json_value_array(node_);
    const json_value_t *cursor = nullptr;
    int cnt = 0;
    json_array_for_each(cursor, arr) {
        if (cnt++ == index) break;
    }
    if (cursor) {
        json_value_t *rem = json_array_remove(cursor, arr);
        json_value_destroy(rem);
    }
}

Json Json::operator[](const char *key) {
    const char *k = key ? key : "";
    if (is_null() && is_root()) {
        to_object();
    } else if (is_object()) {
        json_object_t *obj = json_value_object(node_);
        const json_value_t *res = json_object_find(k, obj);
        if (res != nullptr) {
            return Json(res, node_, std::string(k));
        }
    }

    if (is_placeholder()) {
        destroy_node();
        json_object_t *parent_obj = json_value_object(parent_);
        node_ = const_cast<json_value_t *>(
            json_object_append(parent_obj, parent_key_.c_str(), JSON_VALUE_OBJECT));
        allocated_ = false;
    }

    if (!is_object()) {
        return Json(nullptr, nullptr, "");
    }

    return Json(nullptr, node_, std::string(k));
}

Json Json::operator[](const char *key) const {
    if (!is_object()) {
        return Json(nullptr, nullptr, "");
    }
    const char *k = key ? key : "";
    const json_value_t *res = json_object_find(k, json_value_object(node_));
    if (res != nullptr) {
        return Json(res, node_, std::string(k));
    }
    return Json(nullptr, nullptr, "");
}

Json Json::operator[](const std::string &key) {
    return operator[](key.c_str());
}

Json Json::operator[](const std::string &key) const {
    return operator[](key.c_str());
}

Json Json::operator[](std::string_view key) {
    StackKey sk(key);
    return operator[](sk.c_str);
}

Json Json::operator[](std::string_view key) const {
    if (!is_object()) {
        return Json(nullptr, nullptr, "");
    }
    StackKey sk(key);
    return operator[](sk.c_str);
}

Json Json::operator[](int index) {
    if (!is_array() || index < 0 || index >= static_cast<int>(size())) {
        return Json(nullptr, nullptr, "");
    }
    const json_value_t *val = nullptr;
    json_array_t *arr = json_value_array(node_);
    json_array_for_each(val, arr) {
        if (index == 0) {
            return Json(val, node_, "");
        }
        index--;
    }
    return Json(nullptr, nullptr, "");
}

Json Json::operator[](int index) const {
    if (!is_array() || index < 0 || index >= static_cast<int>(size())) {
        return Json(nullptr, nullptr, "");
    }
    const json_value_t *val = nullptr;
    json_array_t *arr = json_value_array(node_);
    json_array_for_each(val, arr) {
        if (index == 0) {
            return Json(val, node_, "");
        }
        index--;
    }
    return Json(nullptr, nullptr, "");
}

std::string Json::dump(int spaces) const {
    std::string str;
    size_t initial_cap = 64;
    if (spaces == 0 && node_) {
        int t = json_value_type(node_);
        if (t == JSON_VALUE_OBJECT) {
            size_t n = json_object_size(json_value_object(node_));
            if (n * 24 > initial_cap) {
                initial_cap = n * 24;
            }
        } else if (t == JSON_VALUE_ARRAY) {
            size_t n = json_array_size(json_value_array(node_));
            if (n * 16 > initial_cap) {
                initial_cap = n * 16;
            }
        }
    }
    str.reserve(initial_cap);
    value_convert(node_, spaces, 0, &str);
    return str;
}

void Json::string_convert(const char *str, std::string *out) {
    if (!str) {
        out->append("\"\"");
        return;
    }
    out->push_back('"');
    while (*str) {
        switch (*str) {
            case '\r': out->append("\\r"); break;
            case '\n': out->append("\\n"); break;
            case '\f': out->append("\\f"); break;
            case '\b': out->append("\\b"); break;
            case '\"': out->append("\\\""); break;
            case '\t': out->append("\\t"); break;
            case '\\': out->append("\\\\"); break;
            default: {
                unsigned char uc = static_cast<unsigned char>(*str);
                if (uc < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u00%02x", uc);
                    out->append(buf);
                } else {
                    out->push_back(*str);
                }
                break;
            }
        }
        str++;
    }
    out->push_back('"');
}

void Json::number_convert(double number, std::string *out) {
    char buffer[64];
    auto result = std::to_chars(buffer, buffer + sizeof(buffer), number,
                                std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) throw std::runtime_error("JSON number conversion failed");
    out->append(buffer, result.ptr);
}

void Json::array_convert_compact(const json_array_t *arr, std::string *out) {
    out->push_back('[');
    const json_value_t *val = nullptr;
    int n = 0;
    json_array_for_each(val, arr) {
        if (n++ != 0) out->push_back(',');
        value_convert(val, 0, 0, out);
    }
    out->push_back(']');
}

void Json::object_convert_compact(const json_object_t *obj, std::string *out) {
    out->push_back('{');
    const char *name = nullptr;
    const json_value_t *val = nullptr;
    int n = 0;
    json_object_for_each(name, val, obj) {
        if (n++ != 0) out->push_back(',');
        string_convert(name, out);
        out->push_back(':');
        value_convert(val, 0, 0, out);
    }
    out->push_back('}');
}

void Json::array_convert(const json_array_t *arr, int spaces, int depth, std::string *out) {
    if (spaces == 0) {
        array_convert_compact(arr, out);
        return;
    }
    if (json_array_size(arr) == 0) {
        out->append("[]");
        return;
    }
    out->append("[\n");
    const json_value_t *val = nullptr;
    int n = 0;
    std::string pad((depth + 1) * spaces, ' ');
    json_array_for_each(val, arr) {
        if (n++ != 0) out->append(",\n");
        out->append(pad);
        value_convert(val, spaces, depth + 1, out);
    }
    out->push_back('\n');
    out->append(std::string(depth * spaces, ' '));
    out->push_back(']');
}

void Json::object_convert(const json_object_t *obj, int spaces, int depth, std::string *out) {
    if (spaces == 0) {
        object_convert_compact(obj, out);
        return;
    }
    if (json_object_size(obj) == 0) {
        out->append("{}");
        return;
    }
    out->append("{\n");
    const char *name = nullptr;
    const json_value_t *val = nullptr;
    int n = 0;
    std::string pad((depth + 1) * spaces, ' ');
    json_object_for_each(name, val, obj) {
        if (n++ != 0) out->append(",\n");
        out->append(pad);
        string_convert(name, out);
        out->append(": ");
        value_convert(val, spaces, depth + 1, out);
    }
    out->push_back('\n');
    out->append(std::string(depth * spaces, ' '));
    out->push_back('}');
}

void Json::value_convert(const json_value_t *val, int spaces, int depth, std::string *out) {
    if (!val) {
        out->append("null");
        return;
    }
    switch (json_value_type(val)) {
        case JSON_VALUE_STRING:
            string_convert(json_value_string(val), out);
            break;
        case JSON_VALUE_NUMBER:
            number_convert(json_value_number(val), out);
            break;
        case JSON_VALUE_OBJECT:
            object_convert(json_value_object(val), spaces, depth, out);
            break;
        case JSON_VALUE_ARRAY:
            array_convert(json_value_array(val), spaces, depth, out);
            break;
        case JSON_VALUE_TRUE:
            out->append("true");
            break;
        case JSON_VALUE_FALSE:
            out->append("false");
            break;
        case JSON_VALUE_NULL:
            out->append("null");
            break;
        default:
            out->append("null");
            break;
    }
}

Json::IteratorBase::IteratorBase(const json_value_t *val)
    : val_(val), json_(new Json(nullptr, val, "")) {}

Json::IteratorBase::~IteratorBase() {
    delete json_;
}

Json::IteratorBase::IteratorBase(const IteratorBase &other)
    : val_(other.val_), key_(other.key_),
      json_(new Json(other.json_ ? other.json_->node_ : nullptr,
                     other.val_,
                     other.json_ ? other.json_->parent_key_ : "")) {}

Json::IteratorBase &Json::IteratorBase::operator=(const IteratorBase &other) {
    if (this == &other) return *this;
    val_ = other.val_;
    key_ = other.key_;
    delete json_;
    json_ = new Json(other.json_ ? other.json_->node_ : nullptr,
                     other.val_,
                     other.json_ ? other.json_->parent_key_ : "");
    return *this;
}

Json::IteratorBase::IteratorBase(IteratorBase &&other) noexcept
    : val_(other.val_), key_(other.key_), json_(other.json_) {
    other.val_ = nullptr;
    other.key_ = nullptr;
    other.json_ = nullptr;
}

Json::IteratorBase &Json::IteratorBase::operator=(IteratorBase &&other) noexcept {
    if (this == &other) return *this;
    delete json_;
    val_ = other.val_;
    key_ = other.key_;
    json_ = other.json_;
    other.val_ = nullptr;
    other.key_ = nullptr;
    other.json_ = nullptr;
    return *this;
}

Json &Json::IteratorBase::operator*() const {
    if (val_ && json_value_type(val_) == JSON_VALUE_OBJECT && json_->node_) {
        json_->parent_key_ = json_object_value_name(json_->node_, json_value_object(val_));
    } else {
        json_->parent_key_.clear();
    }
    json_->parent_ = val_;
    return *json_;
}

Json *Json::IteratorBase::operator->() const {
    if (val_ && json_value_type(val_) == JSON_VALUE_OBJECT && json_->node_) {
        json_->parent_key_ = json_object_value_name(json_->node_, json_value_object(val_));
    } else {
        json_->parent_key_.clear();
    }
    json_->parent_ = val_;
    return json_;
}

bool Json::IteratorBase::operator==(const IteratorBase &other) const {
    if (!json_ && !other.json_) return true;
    if (!json_ || !other.json_) return false;
    return json_->node_ == other.json_->node_;
}

void Json::iterator::set_begin() {
    json_->node_ = nullptr;
    key_ = nullptr;
    forward();
}

void Json::iterator::set_end() {
    json_->node_ = nullptr;
    key_ = nullptr;
}

bool Json::iterator::is_end() const {
    return json_->node_ == nullptr;
}

void Json::iterator::forward() {
    if (!val_) return;
    int t = json_value_type(val_);
    if (t == JSON_VALUE_OBJECT) {
        const json_object_t *obj = json_value_object(val_);
        json_->node_ = const_cast<json_value_t *>(json_object_next_value(json_->node_, obj));
        if (json_->node_) {
            key_ = json_object_value_name(json_->node_, obj);
            json_->parent_ = val_;
            json_->parent_key_ = key_;
        } else {
            key_ = nullptr;
            json_->parent_ = nullptr;
            json_->parent_key_.clear();
        }
    } else if (t == JSON_VALUE_ARRAY) {
        const json_array_t *arr = json_value_array(val_);
        json_->node_ = const_cast<json_value_t *>(json_array_next_value(json_->node_, arr));
        key_ = nullptr;
        if (json_->node_) {
            json_->parent_ = val_;
        } else {
            json_->parent_ = nullptr;
        }
        json_->parent_key_.clear();
    }
}

Json::iterator &Json::iterator::operator++() {
    if (is_end()) return *this;
    forward();
    return *this;
}

Json::iterator Json::iterator::operator++(int) {
    iterator old = *this;
    if (!is_end()) forward();
    return old;
}

void Json::reverse_iterator::set_rbegin() {
    json_->node_ = nullptr;
    key_ = nullptr;
    backward();
}

void Json::reverse_iterator::set_rend() {
    json_->node_ = nullptr;
    key_ = nullptr;
}

bool Json::reverse_iterator::is_rend() const {
    return json_->node_ == nullptr;
}

void Json::reverse_iterator::backward() {
    if (!val_) return;
    int t = json_value_type(val_);
    if (t == JSON_VALUE_OBJECT) {
        const json_object_t *obj = json_value_object(val_);
        json_->node_ = const_cast<json_value_t *>(json_object_prev_value(json_->node_, obj));
        if (json_->node_) {
            key_ = json_object_value_name(json_->node_, obj);
            json_->parent_ = val_;
            json_->parent_key_ = key_;
        } else {
            key_ = nullptr;
            json_->parent_ = nullptr;
            json_->parent_key_.clear();
        }
    } else if (t == JSON_VALUE_ARRAY) {
        const json_array_t *arr = json_value_array(val_);
        json_->node_ = const_cast<json_value_t *>(json_array_prev_value(json_->node_, arr));
        key_ = nullptr;
        if (json_->node_) {
            json_->parent_ = val_;
        } else {
            json_->parent_ = nullptr;
        }
        json_->parent_key_.clear();
    }
}

Json::reverse_iterator &Json::reverse_iterator::operator++() {
    if (is_rend()) return *this;
    backward();
    return *this;
}

Json::reverse_iterator Json::reverse_iterator::operator++(int) {
    reverse_iterator old = *this;
    if (!is_rend()) backward();
    return old;
}

Json::iterator Json::begin() const {
    iterator it(node_);
    it.set_begin();
    return it;
}

Json::iterator Json::end() const {
    iterator it(node_);
    it.set_end();
    return it;
}

Json::reverse_iterator Json::rbegin() const {
    reverse_iterator it(node_);
    it.set_rbegin();
    return it;
}

Json::reverse_iterator Json::rend() const {
    reverse_iterator it(node_);
    it.set_rend();
    return it;
}

} // namespace kale
