#include "kale/core/context.h"
#include "kale/core/router.h"
#include <new>
#include "stream_reply_adapter.h"
#include <arpa/inet.h>
#include <charconv>
#include <cstdint>
#include <array>
#include <limits>
#include <stdexcept>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"

namespace kale {

namespace {

inline std::string_view trim_ows(std::string_view str) noexcept {
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) {
        str.remove_prefix(1);
    }
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) {
        str.remove_suffix(1);
    }
    return str;
}
inline std::string_view unquote_cookie_value(std::string_view v) noexcept {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v.remove_prefix(1);
        v.remove_suffix(1);
    }
    return v;
}
void parse_cookie_pairs(std::string_view input, std::map<std::string, std::string, std::less<>> &cookies) {
    while (!input.empty()) {
        size_t end = input.find(';');
        if (end == std::string_view::npos) end = input.size();
        std::string_view pair = trim_ows(input.substr(0, end));
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq != std::string_view::npos) {
                std::string_view key = trim_ows(pair.substr(0, eq));
                std::string_view value = unquote_cookie_value(trim_ows(pair.substr(eq + 1)));
                if (!key.empty()) {
                    cookies.emplace(std::string(key), std::string(value));
                }
            }
        }
        if (end == input.size()) break;
        input.remove_prefix(end + 1);
    }
}

inline bool url_decode_matches(std::string_view encoded, std::string_view target) noexcept {
    if (encoded.find('%') == std::string_view::npos && encoded.find('+') == std::string_view::npos) {
        return encoded == target;
    }

    auto hex_to_val = [](char ch) noexcept -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };

    size_t i = 0;
    size_t j = 0;
    while (i < encoded.size() && j < target.size()) {
        char c = encoded[i];
        if (c == '%' && i + 2 < encoded.size()) {
            int h1 = hex_to_val(encoded[i + 1]);
            int h2 = hex_to_val(encoded[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                c = static_cast<char>((h1 << 4) | h2);
                i += 2;
            }
        } else if (c == '+') {
            c = ' ';
        }
        if (c != target[j]) return false;
        ++i;
        ++j;
    }
    return i == encoded.size() && j == target.size();
}

static bool aliases_output_body(const protocol::HttpResponse &raw, std::string_view payload) noexcept {
    if (payload.empty() || raw.get_output_body_size() == 0) return false;
    const uintptr_t p_start = reinterpret_cast<uintptr_t>(payload.data());
    const uintptr_t p_end = p_start + payload.size();

    constexpr size_t FIXED_BLOCK_CAP = 32;
    std::array<const void *, FIXED_BLOCK_CAP> stack_bufs;
    std::array<size_t, FIXED_BLOCK_CAP> stack_sizes;
    size_t count = raw.get_output_body_blocks(stack_bufs.data(), stack_sizes.data(), FIXED_BLOCK_CAP);
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t b_start = reinterpret_cast<uintptr_t>(stack_bufs[i]);
        const uintptr_t b_end = b_start + stack_sizes[i];
        if (p_start < b_end && p_end > b_start) {
            return true;
        }
    }
    if (count < FIXED_BLOCK_CAP) {
        return false;
    }

    size_t cap = 64;
    std::vector<const void *> dyn_bufs;
    std::vector<size_t> dyn_sizes;
    while (true) {
        try {
            dyn_bufs.resize(cap);
            dyn_sizes.resize(cap);
            size_t n = raw.get_output_body_blocks(dyn_bufs.data(), dyn_sizes.data(), cap);
            for (size_t i = 0; i < n; ++i) {
                const uintptr_t b_start = reinterpret_cast<uintptr_t>(dyn_bufs[i]);
                const uintptr_t b_end = b_start + dyn_sizes[i];
                if (p_start < b_end && p_end > b_start) {
                    return true;
                }
            }
            if (n < cap) {
                return false;
            }
            if (cap > std::numeric_limits<size_t>::max() / 2) {
                return true;
            }
            cap *= 2;
        } catch (const std::bad_alloc &) {
            return true;
        }
    }
}
} // namespace

bool get_peer(ServerContext &ctx, sockaddr_storage &address, socklen_t &length) noexcept {
    auto *task = ctx.get_task(); // Borrowed: owned by the request series.
    length = sizeof(address);
    return task && task->get_peer_addr(reinterpret_cast<sockaddr *>(&address), &length) == 0;
}

Context::Context(ServerContext ctx)
    : ctx_(std::move(ctx)) {
    init();
}

Context::~Context() {
    // A destructor cannot await transport completion. Never pretend a partial
    // response succeeded by emitting an unchecked final chunk.
    if (stream_headers_sent_ && !stream_ended_) abort_stream();
}

Context::Context(Context &&other) noexcept
    : ctx_(std::move(other.ctx_)),
      replied_(other.replied_),
      stream_headers_sent_(other.stream_headers_sent_),
      stream_ended_(other.stream_ended_),
      stream_fixed_length_(other.stream_fixed_length_),
      stream_expected_(other.stream_expected_),
      stream_sent_(other.stream_sent_),
      stream_reply_(other.stream_reply_),
      stream_adapter_(std::move(other.stream_adapter_)),
      method_(other.method_),
      method_str_(other.method_str_),
      raw_uri_(other.raw_uri_),
      path_(other.path_),
      route_params_(std::move(other.route_params_)),
      matched_endpoint_(other.matched_endpoint_),
      chunked_body_storage_(std::move(other.chunked_body_storage_)),
      req_body_(other.req_body_),
      attachments_(std::move(other.attachments_)),
      status_code_(other.status_code_),
      error_formatter_(other.error_formatter_),
      error_policy_(other.error_policy_) {
    if (!chunked_body_storage_.empty()) req_body_ = chunked_body_storage_;
    other.replied_ = true;
    other.matched_endpoint_ = nullptr;
    other.stream_reply_ = nullptr;
    other.stream_ended_ = true;
    other.raw_uri_ = {};
    other.path_ = {};
    other.method_str_ = {};
    other.error_formatter_ = nullptr;
    other.error_policy_ = nullptr;
}

Context &Context::operator=(Context &&other) noexcept {
    if (this != &other) {
        if (stream_headers_sent_ && !stream_ended_) abort_stream();
        ctx_ = std::move(other.ctx_);
        replied_ = other.replied_;
        stream_headers_sent_ = other.stream_headers_sent_;
        stream_ended_ = other.stream_ended_;
        stream_fixed_length_ = other.stream_fixed_length_;
        stream_expected_ = other.stream_expected_;
        stream_sent_ = other.stream_sent_;
        stream_reply_ = other.stream_reply_;
        stream_adapter_ = std::move(other.stream_adapter_);
        method_ = other.method_;
        method_str_ = other.method_str_;
        raw_uri_ = other.raw_uri_;
        path_ = other.path_;
        route_params_ = std::move(other.route_params_);
        matched_endpoint_ = other.matched_endpoint_;
        chunked_body_storage_ = std::move(other.chunked_body_storage_);
        req_body_ = other.req_body_;
        if (!chunked_body_storage_.empty()) req_body_ = chunked_body_storage_;
        attachments_ = std::move(other.attachments_);
        status_code_ = other.status_code_;
        error_formatter_ = other.error_formatter_;
        error_policy_ = other.error_policy_;
        other.replied_ = true;
        other.matched_endpoint_ = nullptr;
        other.stream_reply_ = nullptr;
        other.stream_ended_ = true;
        other.raw_uri_ = {};
        other.path_ = {};
        other.method_str_ = {};
        other.error_formatter_ = nullptr;
        other.error_policy_ = nullptr;
    }
    return *this;
}

void Context::init() {
    protocol::HttpRequest &raw = raw_req();
    const char *method_cstr = raw.get_method();
    if (method_cstr) {
        method_str_ = method_cstr;
        method_ = string_to_method(method_str_);
    }
    const char *uri_cstr = raw.get_request_uri();
    if (uri_cstr) {
        raw_uri_ = uri_cstr;
        const size_t qpos = raw_uri_.find('?');
        path_ = (qpos == std::string_view::npos) ? raw_uri_ : raw_uri_.substr(0, qpos);
    }
    if (!raw.is_chunked()) {
        const void *body_ptr = nullptr;
        size_t body_len = 0;
        raw.get_parsed_body(&body_ptr, &body_len);
        if (body_ptr && body_len > 0) req_body_ = std::string_view(static_cast<const char*>(body_ptr), body_len);
    }
}

void Context::prepare_chunked_body_if_needed() {
    protocol::HttpRequest &raw = raw_req();
    if (raw.is_chunked() && chunked_body_storage_.empty()) {
        chunked_body_storage_ = protocol::HttpUtil::decode_chunked_body(&raw);
        req_body_ = chunked_body_storage_;
    }
}

std::string_view Context::content_type() const noexcept {
    protocol::HttpMessageHeader h;
    h.name = "Content-Type";
    h.name_len = 12;
    protocol::HttpHeaderCursor cursor(&raw_req());
    if (cursor.find(&h)) {
        return std::string_view(static_cast<const char*>(h.value), h.value_len);
    }
    return "";
}

bool Context::has_header(std::string_view key) const noexcept {
    if (key.empty()) return false;
    protocol::HttpMessageHeader h;
    h.name = key.data();
    h.name_len = key.size();
    protocol::HttpHeaderCursor cursor(&raw_req());
    return cursor.find(&h);
}

std::string_view Context::header_view(std::string_view key) const noexcept {
    if (key.empty()) return {};
    protocol::HttpMessageHeader h;
    h.name = key.data();
    h.name_len = key.size();
    protocol::HttpHeaderCursor cursor(&raw_req());
    if (!cursor.find(&h)) return {};
    return {static_cast<const char *>(h.value), h.value_len};
}

std::string Context::header(std::string_view key) const {
    if (key.empty()) return "";
    protocol::HttpMessageHeader h;
    h.name = key.data();
    h.name_len = key.size();
    protocol::HttpHeaderCursor cursor(&raw_req());
    if (!cursor.find(&h)) {
        return "";
    }
    protocol::HttpMessageHeader h_next = h;
    if (!cursor.find(&h_next)) {
        return std::string(static_cast<const char*>(h.value), h.value_len);
    }
    std::string result(static_cast<const char*>(h.value), h.value_len);
    const bool is_cookie = (!CaseInsensitiveLess{}(key, "Cookie") && !CaseInsensitiveLess{}("Cookie", key));
    const std::string_view sep = is_cookie ? "; " : ", ";
    do {
        result.append(sep);
        result.append(static_cast<const char*>(h_next.value), h_next.value_len);
    } while (cursor.find(&h_next));
    return result;
}

std::map<std::string, std::string, CaseInsensitiveLess> Context::headers() const {
    std::map<std::string, std::string, CaseInsensitiveLess> result;
    protocol::HttpHeaderCursor cursor(&raw_req());
    protocol::HttpMessageHeader h;
    while (cursor.next(&h)) {
        std::string_view name(static_cast<const char*>(h.name), h.name_len);
        std::string_view value(static_cast<const char*>(h.value), h.value_len);
        auto it = result.find(name);
        if (it != result.end()) {
            if (!CaseInsensitiveLess{}(name, "Cookie") && !CaseInsensitiveLess{}("Cookie", name)) {
                it->second.append("; ").append(value);
            } else {
                it->second.append(", ").append(value);
            }
        } else {
            result.emplace(std::string(name), std::string(value));
        }
    }
    return result;
}

void Context::headers(std::span<const std::string_view> keys, std::span<std::string> out) const {
    if (keys.size() != out.size()) {
        throw std::invalid_argument("Context::headers: keys and out span sizes must match");
    }
    if (keys.empty()) {
        return;
    }
    for (auto &s : out) {
        s.clear();
    }

    const size_t nkeys = keys.size();
    constexpr size_t STACK_FLAG_CAP = 64;
    std::array<uint8_t, STACK_FLAG_CAP> stack_seen;
    std::vector<uint8_t> dyn_seen;
    std::span<uint8_t> seen;

    if (nkeys <= STACK_FLAG_CAP) {
        stack_seen.fill(0);
        seen = std::span<uint8_t>(stack_seen.data(), nkeys);
    } else {
        dyn_seen.assign(nkeys, 0);
        seen = std::span<uint8_t>(dyn_seen.data(), nkeys);
    }

    protocol::HttpHeaderCursor cursor(&raw_req());
    protocol::HttpMessageHeader h;
    while (cursor.next(&h)) {
        const std::string_view h_name(static_cast<const char*>(h.name), h.name_len);
        const std::string_view h_val(static_cast<const char*>(h.value), h.value_len);

        for (size_t i = 0; i < nkeys; ++i) {
            if (case_insensitive_equal(keys[i], h_name)) {
                if (!seen[i]) {
                    out[i].assign(h_val);
                    seen[i] = 1;
                } else {
                    const bool is_cookie = (!CaseInsensitiveLess{}(keys[i], "Cookie") && !CaseInsensitiveLess{}("Cookie", keys[i]));
                    if (is_cookie) {
                        out[i].append("; ");
                    } else {
                        out[i].append(", ");
                    }
                    out[i].append(h_val);
                }
            }
        }
    }
}

bool Context::has_param(std::string_view key) const noexcept {
    for (const auto &[k, v] : route_params_) {
        if (k == key) {
            return true;
        }
    }
    return false;
}

std::string_view Context::param(std::string_view key) const noexcept {
    for (const auto &[k, v] : route_params_) {
        if (k == key) {
            return v;
        }
    }
    return "";
}

std::string_view Context::default_param(std::string_view key, std::string_view default_val) const noexcept {
    for (const auto &[k, v] : route_params_) {
        if (k == key) {
            return v;
        }
    }
    return default_val;
}

bool Context::has_query(std::string_view key) const noexcept {
    if (key.empty()) return false;
    const size_t qpos = raw_uri_.find('?');
    if (qpos == std::string_view::npos) return false;
    std::string_view q = raw_uri_.substr(qpos + 1);
    while (!q.empty()) {
        size_t amp = q.find('&');
        std::string_view field = (amp != std::string_view::npos) ? q.substr(0, amp) : q;
        if (!field.empty()) {
            size_t eq = field.find('=');
            std::string_view raw_k = (eq != std::string_view::npos) ? field.substr(0, eq) : field;
            if (!raw_k.empty() && url_decode_matches(raw_k, key)) {
                return true;
            }
        }
        if (amp == std::string_view::npos) break;
        q.remove_prefix(amp + 1);
    }
    return false;
}

std::string Context::query(std::string_view key) const {
    return default_query(key, std::string_view{});
}

std::string Context::default_query(std::string_view key, std::string_view default_val) const {
    if (key.empty()) return std::string(default_val);
    const size_t qpos = raw_uri_.find('?');
    if (qpos == std::string_view::npos) return std::string(default_val);
    std::string_view q = raw_uri_.substr(qpos + 1);
    while (!q.empty()) {
        size_t amp = q.find('&');
        std::string_view field = (amp != std::string_view::npos) ? q.substr(0, amp) : q;
        if (!field.empty()) {
            size_t eq = field.find('=');
            std::string_view raw_k = (eq != std::string_view::npos) ? field.substr(0, eq) : field;
            if (!raw_k.empty() && url_decode_matches(raw_k, key)) {
                std::string_view raw_v = (eq != std::string_view::npos) ? field.substr(eq + 1) : std::string_view{};
                return url_decode(raw_v);
            }
        }
        if (amp == std::string_view::npos) break;
        q.remove_prefix(amp + 1);
    }
    return std::string(default_val);
}

std::string Context::default_query(std::string_view key, std::string &&default_val) const {
    if (key.empty()) return std::move(default_val);
    const size_t qpos = raw_uri_.find('?');
    if (qpos == std::string_view::npos) return std::move(default_val);
    std::string_view q = raw_uri_.substr(qpos + 1);
    while (!q.empty()) {
        size_t amp = q.find('&');
        std::string_view field = (amp != std::string_view::npos) ? q.substr(0, amp) : q;
        if (!field.empty()) {
            size_t eq = field.find('=');
            std::string_view raw_k = (eq != std::string_view::npos) ? field.substr(0, eq) : field;
            if (!raw_k.empty() && url_decode_matches(raw_k, key)) {
                std::string_view raw_v = (eq != std::string_view::npos) ? field.substr(eq + 1) : std::string_view{};
                return url_decode(raw_v);
            }
        }
        if (amp == std::string_view::npos) break;
        q.remove_prefix(amp + 1);
    }
    return std::move(default_val);
}

std::map<std::string, std::string, std::less<>> Context::query_params() const {
    const size_t qpos = raw_uri_.find('?');
    if (qpos == std::string_view::npos) return {};
    return split_query(raw_uri_.substr(qpos + 1));
}

void Context::queries(std::span<const std::string_view> keys, std::span<std::string> out) const {
    if (keys.size() != out.size()) {
        throw std::invalid_argument("Context::queries: keys and out span sizes must match");
    }
    if (keys.empty()) {
        return;
    }
    for (auto &s : out) {
        s.clear();
    }

    const size_t qpos = raw_uri_.find('?');
    if (qpos == std::string_view::npos) {
        return;
    }
    std::string_view q = raw_uri_.substr(qpos + 1);
    if (q.empty()) {
        return;
    }

    const size_t nkeys = keys.size();
    constexpr size_t STACK_FLAG_CAP = 64;
    std::array<uint8_t, STACK_FLAG_CAP> stack_seen;
    std::vector<uint8_t> dyn_seen;
    std::span<uint8_t> seen;

    if (nkeys <= STACK_FLAG_CAP) {
        stack_seen.fill(0);
        seen = std::span<uint8_t>(stack_seen.data(), nkeys);
    } else {
        dyn_seen.assign(nkeys, 0);
        seen = std::span<uint8_t>(dyn_seen.data(), nkeys);
    }

    size_t seen_count = 0;
    while (!q.empty()) {
        size_t amp = q.find('&');
        std::string_view field = (amp != std::string_view::npos) ? q.substr(0, amp) : q;
        if (!field.empty()) {
            size_t eq = field.find('=');
            std::string_view raw_k = (eq != std::string_view::npos) ? field.substr(0, eq) : field;
            if (!raw_k.empty()) {
                std::string_view raw_v = (eq != std::string_view::npos) ? field.substr(eq + 1) : std::string_view{};
                std::optional<std::string> decoded_v;

                for (size_t i = 0; i < nkeys; ++i) {
                    if (!seen[i] && url_decode_matches(raw_k, keys[i])) {
                        if (!decoded_v) {
                            decoded_v = url_decode(raw_v);
                        }
                        out[i] = *decoded_v;
                        seen[i] = 1;
                        ++seen_count;
                    }
                }
                if (seen_count == nkeys) {
                    break;
                }
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        q.remove_prefix(amp + 1);
    }
}

bool Context::has_cookie(std::string_view name) const noexcept {
    if (name.empty()) return false;
    protocol::HttpHeaderCursor cursor(&raw_req());
    protocol::HttpMessageHeader h;
    h.name = "Cookie";
    h.name_len = 6;
    while (cursor.find(&h)) {
        std::string_view input(static_cast<const char*>(h.value), h.value_len);
        while (!input.empty()) {
            size_t end = input.find(';');
            if (end == std::string_view::npos) end = input.size();
            std::string_view pair = trim_ows(input.substr(0, end));
            if (!pair.empty()) {
                size_t eq = pair.find('=');
                if (eq != std::string_view::npos) {
                    std::string_view key = trim_ows(pair.substr(0, eq));
                    if (key == name) return true;
                }
            }
            if (end == input.size()) break;
            input.remove_prefix(end + 1);
        }
    }
    return false;
}

std::string_view Context::cookie(std::string_view name) const noexcept {
    return default_cookie(name, std::string_view{});
}

std::string_view Context::default_cookie(std::string_view name, std::string_view default_val) const noexcept {
    if (name.empty()) return default_val;
    protocol::HttpHeaderCursor cursor(&raw_req());
    protocol::HttpMessageHeader h;
    h.name = "Cookie";
    h.name_len = 6;
    while (cursor.find(&h)) {
        std::string_view input(static_cast<const char*>(h.value), h.value_len);
        while (!input.empty()) {
            size_t end = input.find(';');
            if (end == std::string_view::npos) end = input.size();
            std::string_view pair = trim_ows(input.substr(0, end));
            if (!pair.empty()) {
                size_t eq = pair.find('=');
                if (eq != std::string_view::npos) {
                    std::string_view key = trim_ows(pair.substr(0, eq));
                    if (key == name) {
                        return unquote_cookie_value(trim_ows(pair.substr(eq + 1)));
                    }
                }
            }
            if (end == input.size()) break;
            input.remove_prefix(end + 1);
        }
    }
    return default_val;
}

std::map<std::string, std::string, std::less<>> Context::cookies() const {
    std::map<std::string, std::string, std::less<>> result;
    protocol::HttpHeaderCursor cursor(&raw_req());
    protocol::HttpMessageHeader h;
    h.name = "Cookie";
    h.name_len = 6;
    while (cursor.find(&h)) {
        parse_cookie_pairs(std::string_view(static_cast<const char*>(h.value), h.value_len), result);
    }
    return result;
}


bool Context::is_json() const noexcept {
    std::string_view ct = content_type();
    return (ct.find("application/json") != std::string_view::npos ||
            ct.find("+json") != std::string_view::npos);
}

std::string_view Context::route_name() const noexcept {
    return matched_endpoint_ ? std::string_view(matched_endpoint_->name) : "";
}

std::string_view Context::route_pattern() const noexcept {
    return matched_endpoint_ ? std::string_view(matched_endpoint_->pattern) : "";
}

std::string_view Context::route_tag(std::string_view key) const noexcept {
    if (matched_endpoint_) {
        const auto eit = matched_endpoint_->tags.find(key);
        if (eit != matched_endpoint_->tags.end()) {
            return eit->second;
        }
    }
    return "";
}

bool Context::has_route_tag(std::string_view key) const noexcept {
    if (matched_endpoint_) {
        return matched_endpoint_->tags.find(key) != matched_endpoint_->tags.end();
    }
    return false;
}

std::string Context::peer_addr() const {
    auto *task = const_cast<ServerContext &>(ctx_).get_task(); // Borrowed: request series owns task.
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    if (!task || task->get_peer_addr(reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) return "Unknown";
    const void *binary = nullptr;
    if (addr.ss_family == AF_INET && addr_len >= sizeof(sockaddr_in)) binary = &reinterpret_cast<sockaddr_in *>(&addr)->sin_addr;
    else if (addr.ss_family == AF_INET6 && addr_len >= sizeof(sockaddr_in6)) binary = &reinterpret_cast<sockaddr_in6 *>(&addr)->sin6_addr;
    else return "Unknown";
    char text[INET6_ADDRSTRLEN];
    return inet_ntop(addr.ss_family, binary, text, sizeof(text)) ? text : "Unknown";
}

unsigned short Context::peer_port() const noexcept {
    auto *task = const_cast<ServerContext &>(ctx_).get_task(); // Borrowed: request series owns task.
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    if (!task || task->get_peer_addr(reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) return 0;
    if (addr.ss_family == AF_INET && addr_len >= sizeof(sockaddr_in)) return ntohs(reinterpret_cast<sockaddr_in *>(&addr)->sin_port);
    if (addr.ss_family == AF_INET6 && addr_len >= sizeof(sockaddr_in6)) return ntohs(reinterpret_cast<sockaddr_in6 *>(&addr)->sin6_port);
    return 0;
}



void Context::sync_status_to_raw() {
    protocol::HttpResponse &raw = raw_resp();
    const char *ver = raw.get_http_version();
    if (!ver || !*ver) {
        raw.set_http_version("HTTP/1.1");
    }
    int final_status = status_code_;
    if (!is_valid_status_code(final_status)) {
        final_status = static_cast<int>(HttpStatus::InternalServerError);
    }
    switch (final_status) {
    case 100: raw.set_status_code("100"); break;
    case 101: raw.set_status_code("101"); break;
    case 200: raw.set_status_code("200"); break;
    case 201: raw.set_status_code("201"); break;
    case 202: raw.set_status_code("202"); break;
    case 204: raw.set_status_code("204"); break;
    case 205: raw.set_status_code("205"); break;
    case 206: raw.set_status_code("206"); break;
    case 300: raw.set_status_code("300"); break;
    case 301: raw.set_status_code("301"); break;
    case 302: raw.set_status_code("302"); break;
    case 303: raw.set_status_code("303"); break;
    case 304: raw.set_status_code("304"); break;
    case 307: raw.set_status_code("307"); break;
    case 308: raw.set_status_code("308"); break;
    case 400: raw.set_status_code("400"); break;
    case 401: raw.set_status_code("401"); break;
    case 403: raw.set_status_code("403"); break;
    case 404: raw.set_status_code("404"); break;
    case 405: raw.set_status_code("405"); break;
    case 406: raw.set_status_code("406"); break;
    case 408: raw.set_status_code("408"); break;
    case 409: raw.set_status_code("409"); break;
    case 410: raw.set_status_code("410"); break;
    case 411: raw.set_status_code("411"); break;
    case 413: raw.set_status_code("413"); break;
    case 414: raw.set_status_code("414"); break;
    case 415: raw.set_status_code("415"); break;
    case 416: raw.set_status_code("416"); break;
    case 422: raw.set_status_code("422"); break;
    case 429: raw.set_status_code("429"); break;
    case 500: raw.set_status_code("500"); break;
    case 501: raw.set_status_code("501"); break;
    case 502: raw.set_status_code("502"); break;
    case 503: raw.set_status_code("503"); break;
    case 504: raw.set_status_code("504"); break;
    case 505: raw.set_status_code("505"); break;
    default: {
        char status_buf[8];
        auto [ptr, ec] = std::to_chars(status_buf, status_buf + sizeof(status_buf) - 1, final_status);
        *ptr = '\0';
        raw.set_status_code(status_buf);
        break;
    }
    }

    std::string_view phrase = status_phrase(final_status);
    if (!is_valid_reason_phrase(phrase)) {
        phrase = "Internal Server Error";
    }
    raw.set_reason_phrase(phrase.data());
}

Context &Context::set_status(int status_code) noexcept {
    status_code_ = status_code;
    return *this;
}

Context &Context::set_status(HttpStatus status) noexcept {
    status_code_ = static_cast<int>(status);
    return *this;
}

Context &Context::set_header(std::string_view key, std::string_view value) {
    if (is_valid_header_name(key) && is_valid_header_value(value)) {
        protocol::HttpMessageHeader h;
        h.name = key.data();
        h.name_len = key.size();
        h.value = value.data();
        h.value_len = value.size();
        raw_resp().set_header(&h);
    }
    return *this;
}

Context &Context::set_content_type(std::string_view content_type) {
    return set_header("Content-Type", content_type);
}

bool Context::has_resp_header(std::string_view key) const noexcept {
    if (key.empty()) return false;
    protocol::HttpMessageHeader h;
    h.name = key.data();
    h.name_len = key.size();
    protocol::HttpHeaderCursor cursor(&raw_resp());
    return cursor.find(&h);
}

std::string_view Context::resp_header(std::string_view key) const noexcept {
    if (key.empty()) return "";
    protocol::HttpMessageHeader h;
    h.name = key.data();
    h.name_len = key.size();
    protocol::HttpHeaderCursor cursor(&raw_resp());
    if (cursor.find(&h)) {
        return std::string_view(static_cast<const char*>(h.value), h.value_len);
    }
    return "";
}

bool Context::erase_resp_header(std::string_view key) noexcept {
    if (key.empty()) return false;
    protocol::HttpMessageHeader h;
    h.name = key.data();
    h.name_len = key.size();
    protocol::HttpHeaderCursor cursor(&raw_resp());
    bool erased = false;
    while (cursor.find(&h)) {
        cursor.erase();
        erased = true;
    }
    return erased;
}


Context &Context::body(std::string_view payload) {
    if (payload.empty()) {
        raw_resp().clear_output_body();
        return *this;
    }
    if (raw_resp().get_output_body_size() == 0) {
        if (!raw_resp().append_output_body(payload.data(), payload.size())) {
            throw std::bad_alloc();
        }
        return *this;
    }
    if (aliases_output_body(raw_resp(), payload)) {
        std::string safe_copy(payload);
        raw_resp().clear_output_body();
        if (!raw_resp().append_output_body(safe_copy.data(), safe_copy.size())) {
            throw std::bad_alloc();
        }
        return *this;
    }
    raw_resp().clear_output_body();
    if (!raw_resp().append_output_body(payload.data(), payload.size())) {
        throw std::bad_alloc();
    }
    return *this;
}

Context &Context::string(std::string_view text) {
    body(text);
    if (!has_resp_header("Content-Type")) {
        set_content_type("text/plain; charset=utf-8");
    }
    return *this;
}

Context &Context::html(std::string_view html_content) {
    body(html_content);
    set_content_type("text/html; charset=utf-8");
    return *this;
}


Context &Context::redirect(std::string location, int status) {
    set_status(status);
    set_header("Location", location);
    return *this;
}

size_t Context::resp_body_size() const noexcept {
    return raw_resp().get_output_body_size();
}

size_t Context::get_resp_body_blocks(const void *buf[], size_t size[], size_t max) const {
    return raw_resp().get_output_body_blocks(buf, size, max);
}

bool Context::get_resp_body_merged(std::string &out) const {
    return raw_resp().get_output_body_merged(out);
}


coke::NetworkReplyAwaiter Context::reply() {
    assert(!replied_);
    replied_ = true;
    sync_status_to_raw();
    return ctx_.reply();
}


bool Context::start_stream() {
    if (stream_ended_ || replied_) return false;
    if (stream_headers_sent_) return true;
    auto *task = ctx_.get_task();
    if (!task) {
        stream_ended_ = true;
        return false;
    }
    const int status = is_valid_status_code(status_code_) ? status_code_ : 500;
    status_code_ = status;

    if (!has_resp_header("Content-Type")) {
        set_header("Content-Type", "text/plain; charset=utf-8");
    }

    if (has_resp_header("Content-Length")) {
        auto length = resp_header("Content-Length");
        auto result = std::from_chars(length.data(), length.data() + length.size(), stream_expected_);
        if (result.ec != std::errc{} || result.ptr != length.data() + length.size())
            throw std::invalid_argument("Invalid response Content-Length");
        stream_fixed_length_ = true;
        erase_resp_header("Transfer-Encoding");
    } else {
        stream_fixed_length_ = false;
        set_header("Transfer-Encoding", "chunked");
    }

    if (!has_resp_header("Connection")) {
        if (task->get_req()->is_keep_alive()) {
            set_header("Connection", "keep-alive");
        } else {
            set_header("Connection", "close");
            task->set_keep_alive(0);
        }
    } else {
        auto conn_val = resp_header("Connection");
        if (case_insensitive_equal(conn_val, "close")) {
            task->set_keep_alive(0);
        }
    }

    sync_status_to_raw();
    raw_resp().clear_output_body();

    bool is_head = (method_ == HttpMethod::HEAD);
    bool chunked = !stream_fixed_length_ && !is_head;

    StreamReply *reply = task->start_stream_reply(chunked);
    if (!reply) {
        stream_ended_ = true;
        return false;
    }
    stream_reply_ = reply;
    stream_headers_sent_ = true;
    try {
        stream_adapter_ = std::make_unique<StreamReplyAdapter>(reply);
    } catch (...) {
        abort_stream();
        throw;
    }

    return true;
}

coke::Task<bool> Context::send_chunk(std::string_view raw_chunk) {
    if (stream_ended_ || !is_connected()) {
        stream_ended_ = true;
        co_return false;
    }

    if (!stream_headers_sent_ && !start_stream()) {
        stream_ended_ = true;
        co_return false;
    }

    if (raw_chunk.empty()) co_return true;
    if (method_ == HttpMethod::HEAD) co_return true;

    if (stream_fixed_length_) {
        if (raw_chunk.size() > stream_expected_ - stream_sent_) {
            abort_stream();
            co_return false;
        }
        if (!co_await stream_adapter_->async_write(raw_chunk)) {
            abort_stream();
            co_return false;
        }
        stream_sent_ += raw_chunk.size();
        co_return true;
    }

    if (!co_await stream_adapter_->async_write(raw_chunk)) {
        abort_stream();
        co_return false;
    }
    co_return true;
}

coke::Task<bool> Context::send_chunk(std::string raw_chunk) {
    if (stream_ended_ || !is_connected()) {
        stream_ended_ = true;
        co_return false;
    }

    if (!stream_headers_sent_ && !start_stream()) {
        stream_ended_ = true;
        co_return false;
    }

    if (raw_chunk.empty()) co_return true;
    if (method_ == HttpMethod::HEAD) co_return true;

    if (stream_fixed_length_) {
        if (raw_chunk.size() > stream_expected_ - stream_sent_) {
            abort_stream();
            co_return false;
        }
        const size_t sz = raw_chunk.size();
        if (!co_await stream_adapter_->async_write(std::string_view(raw_chunk))) {
            abort_stream();
            co_return false;
        }
        stream_sent_ += sz;
        co_return true;
    }

    if (!co_await stream_adapter_->async_write(std::string_view(raw_chunk))) {
        abort_stream();
        co_return false;
    }
    co_return true;
}

coke::Task<bool> Context::end_chunk() {
    if (stream_ended_) co_return false;
    if (!stream_headers_sent_ && !co_await send_chunk({})) {
        abort_stream();
        co_return false;
    }
    if (stream_fixed_length_) {
        if (method_ != HttpMethod::HEAD && stream_sent_ != stream_expected_) {
            abort_stream();
            co_return false;
        }
    }
    if (stream_adapter_) {
        bool ok = co_await stream_adapter_->async_finish();
        if (!ok) {
            abort_stream();
            co_return false;
        }
    }
    stream_ended_ = true;
    stream_reply_ = nullptr;
    co_return true;
}

void Context::abort_stream() noexcept {
    stream_ended_ = true;
    mark_replied();
    if (stream_adapter_) {
        stream_adapter_->abort();
    } else if (stream_reply_) {
        stream_reply_->abort();
    }
    stream_reply_ = nullptr;
}

bool Context::is_connected() const noexcept {
    if (stream_ended_) return false;
    if (stream_adapter_) {
        return stream_adapter_->is_connected();
    }
    auto *task = const_cast<ServerContext&>(ctx_).get_task();
    return task && !task->closed();
}
void Context::set_stream_close_hook(std::function<void()> hook) {
    if (stream_adapter_) {
        stream_adapter_->set_close_hook(std::move(hook));
    }
}

void Context::clear_stream_close_hook() {
    if (stream_adapter_) {
        stream_adapter_->clear_close_hook();
    }
}


} // namespace kale
