#ifndef KALE_CORE_CONTEXT_H
#define KALE_CORE_CONTEXT_H

#include <any>
#include <exception>
#include <functional>
#include <map>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coke/net/basic_server.h"
#include "coke/task.h"
#include "workflow/HttpMessage.h"

#include "kale/core/cookie.h"
#include "kale/core/http_error.h"
#include "kale/core/http_method.h"
#include "kale/core/http_status.h"
#include "kale/core/http_util.h"
#include "kale/core/error_response.h"
class StreamReply;

namespace kale {

class Json;
class HttpServer;
class Router;
struct RouteEndpoint;
class StreamReplyAdapter;
class StreamAccess;
using ServerContext = coke::ServerContext<protocol::HttpRequest, protocol::HttpResponse>;

class Context {
    friend class HttpServer;
    friend class Router;
    friend class StreamAccess;

public:
    explicit Context(ServerContext ctx);
    ~Context();

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    Context(Context &&) noexcept;
    Context &operator=(Context &&) noexcept;

    ServerContext &coke_context() noexcept { return ctx_; }
    const ServerContext &coke_context() const noexcept { return ctx_; }
    protocol::HttpRequest &raw_req() noexcept { return ctx_.get_req(); }
    const protocol::HttpRequest &raw_req() const noexcept { return const_cast<ServerContext&>(ctx_).get_req(); }
    protocol::HttpResponse &raw_resp() noexcept { return ctx_.get_resp(); }
    const protocol::HttpResponse &raw_resp() const noexcept { return const_cast<ServerContext&>(ctx_).get_resp(); }
    long long seqid() noexcept { return ctx_.get_task() ? ctx_.get_task()->get_task_seq() : 0; }

    HttpMethod method() const noexcept { return method_; }
    std::string_view method_string() const noexcept { return method_str_; }
    std::string_view path() const noexcept { return path_; }
    std::string_view raw_uri() const noexcept { return raw_uri_; }

    bool has_header(std::string_view key) const noexcept;
    std::string header(std::string_view key) const;
    /**
     * Borrows request storage; valid until headers are mutated or Context is destroyed.
     * Duplicate fields are not joined; use header() or headers() for joined values.
     */
    std::string_view header_view(std::string_view key) const noexcept;
    std::map<std::string, std::string, CaseInsensitiveLess> headers() const;
    std::string_view content_type() const noexcept;

    /**
     * Extracts headers in 1-to-1 correspondence with keys, folding duplicates.
     * @throws std::invalid_argument if keys.size() != out.size().
     */
    void headers(std::span<const std::string_view> keys, std::span<std::string> out) const;

    bool has_param(std::string_view key) const noexcept;
    std::string_view param(std::string_view key) const noexcept;
    std::string_view default_param(std::string_view key, std::string_view default_val) const noexcept;

    bool has_query(std::string_view key) const noexcept;
    std::string query(std::string_view key) const;
    std::string default_query(std::string_view key, std::string_view default_val) const;
    std::string default_query(std::string_view key, const char *default_val) const {
        return default_query(key, std::string_view(default_val ? default_val : ""));
    }
    std::string default_query(std::string_view key, std::string &&default_val) const;
    std::map<std::string, std::string, std::less<>> query_params() const;

    /**
     * Extracts URL-decoded query parameters in 1-to-1 correspondence with keys.
     * @throws std::invalid_argument if keys.size() != out.size().
     */
    void queries(std::span<const std::string_view> keys, std::span<std::string> out) const;

    bool has_cookie(std::string_view name) const noexcept;
    std::string_view cookie(std::string_view name) const noexcept;
    std::string_view default_cookie(std::string_view name, std::string_view default_val) const noexcept;
    std::map<std::string, std::string, std::less<>> cookies() const;
    std::string_view req_body() const noexcept { return req_body_; }

    // Throws JsonInputError on malformed input (nested exception).
    template <typename J = Json>
    J parse_json() const {
        try {
            return J::parse(req_body());
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &) {
            std::throw_with_nested(JsonInputError("invalid JSON"));
        }
    }

    bool is_json() const noexcept;

    template <typename DTO, typename J = Json>
    DTO bind_json() const;
    std::string_view route_name() const noexcept;
    std::string_view route_pattern() const noexcept;
    std::string_view route_tag(std::string_view key) const noexcept;
    bool has_route_tag(std::string_view key) const noexcept;
    std::string peer_addr() const;
    unsigned short peer_port() const noexcept;

    template <typename T>
    void set_attachment(T &&data) {
        using RawType = std::decay_t<T>;
        attachments_[std::type_index(typeid(RawType))] = std::make_any<RawType>(std::forward<T>(data));
    }

    template <typename T>
    bool has_attachment() const noexcept {
        return attachments_.find(std::type_index(typeid(T))) != attachments_.end();
    }

    /**
     * Returns nullptr if absent. Valid until erased, overwritten, or Context destroyed.
     */
    template <typename T>
    const T* get_attachment() const noexcept {
        auto it = attachments_.find(std::type_index(typeid(T)));
        if (it == attachments_.end()) {
            return nullptr;
        }
        return std::any_cast<T>(&it->second);
    }

    template <typename T>
    T* get_attachment() noexcept {
        auto it = attachments_.find(std::type_index(typeid(T)));
        if (it == attachments_.end()) {
            return nullptr;
        }
        return std::any_cast<T>(&it->second);
    }

    template <typename T>
    void erase_attachment() noexcept {
        attachments_.erase(std::type_index(typeid(T)));
    }

    void clear_attachments() noexcept {
        attachments_.clear();
    }

    Context &set_status(int status_code) noexcept;
    Context &set_status(HttpStatus status) noexcept;
    int status_code() const noexcept { return status_code_; }
    /**
     * Validates keys and values against CRLF injection; replaces case-insensitively.
     */
    Context &set_header(std::string_view key, std::string_view value);
    Context &set_content_type(std::string_view content_type);

    bool has_resp_header(std::string_view key) const noexcept;

    /**
     * Returns empty view if absent; valid until response headers are mutated or Context destroyed.
     */
    std::string_view resp_header(std::string_view key) const noexcept;

    bool erase_resp_header(std::string_view key) noexcept;

    /**
     * @throws std::invalid_argument if cookie attributes are invalid.
     */
    Context &set_cookie(Cookie cookie);
    Context &set_cookie(std::string name, std::string value);

    /**
     * Replaces response body; safe for self-aliased views.
     */
    Context &body(std::string_view payload);
    Context &string(std::string_view text);
    Context &html(std::string_view html_content);
    template <typename J>
        requires (requires(const J &j) { { j.dump() } -> std::convertible_to<std::string_view>; })
    Context &json(const J &json_data) {
        const auto dumped = json_data.dump();
        body(std::string_view(dumped));
        set_content_type("application/json; charset=utf-8");
        return *this;
    }

    template <typename S>
        requires (!requires(const S &s) { { s.dump() } -> std::convertible_to<std::string_view>; } &&
                  std::is_convertible_v<S, std::string_view>)
    Context &json(S &&text) {
        body(std::string_view(std::forward<S>(text)));
        set_content_type("application/json; charset=utf-8");
        return *this;
    }

    Context &redirect(std::string location, int status = static_cast<int>(HttpStatus::Found));

    size_t resp_body_size() const noexcept;

    /**
     * Block buffer pointers remain valid until the next response body mutation.
     */
    size_t get_resp_body_blocks(const void *buf[], size_t size[], size_t max) const;

    bool get_resp_body_merged(std::string &out) const;
    bool is_replied() const noexcept { return replied_; }
    bool is_stream() const noexcept { return stream_headers_sent_; }
    void mark_replied() noexcept { replied_ = true; }
    coke::NetworkReplyAwaiter reply();

    /**
     * Borrowed views must remain valid until the returned task completes.
     */
    coke::Task<bool> send_chunk(std::string_view raw_chunk);
    coke::Task<bool> send_chunk(std::string raw_chunk);
    coke::Task<bool> send_chunk(const char *raw_chunk) {
        return send_chunk(std::string_view(raw_chunk ? raw_chunk : ""));
    }
    coke::Task<bool> end_chunk();
    void abort_stream() noexcept;
    bool is_connected() const noexcept;
    const ErrorFormatter *error_formatter() const noexcept { return error_formatter_; }
    const ErrorPolicy *error_policy() const noexcept { return error_policy_; }
    void set_error_config(const ErrorFormatter *formatter, const ErrorPolicy *policy) noexcept {
        error_formatter_ = formatter;
        error_policy_ = policy;
    }


private:
    void init();
    void prepare_chunked_body_if_needed();
    void sync_status_to_raw();

    bool start_stream();
    void set_stream_close_hook(std::function<void()> hook);
    void clear_stream_close_hook();
    ServerContext ctx_;
    bool replied_{false};
    bool stream_headers_sent_{false};
    bool stream_ended_{false};
    bool stream_fixed_length_{false};
    size_t stream_expected_{0};
    size_t stream_sent_{0};
    ::StreamReply *stream_reply_{nullptr};
    std::unique_ptr<StreamReplyAdapter> stream_adapter_;

    HttpMethod method_{HttpMethod::UNKNOWN};
    std::string_view method_str_;
    std::string_view raw_uri_;
    std::string_view path_;
    std::vector<std::pair<std::string, std::string>> route_params_;
    // Non-owning pointer to endpoint owned by Router.
    const RouteEndpoint *matched_endpoint_{nullptr};
    std::string chunked_body_storage_;
    std::string_view req_body_;

    std::unordered_map<std::type_index, std::any> attachments_;

    int status_code_{static_cast<int>(HttpStatus::OK)};
    const ErrorFormatter *error_formatter_{nullptr};
    const ErrorPolicy *error_policy_{nullptr};
};

} // namespace kale

#endif // KALE_CORE_CONTEXT_H
