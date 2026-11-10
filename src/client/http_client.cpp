#include "kale/client/http_client.h"
#include "http/base64.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include "coke/condition.h"
#include "coke/sleep.h"
#include "kale/http/json.h"
#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/URIParser.h"
#include "workflow/StringUtil.h"
#include "workflow/WFHttpChunkedClient.h"
#include "workflow/WFTaskError.h"
#include "workflow/WFTaskFactory.h"
namespace kale {


struct StreamState {
    std::mutex mutex;
    coke::Condition cond_headers;
    coke::Condition cond_data;

    std::string request_body;
    std::string response_reason;
    std::vector<std::pair<std::string, std::string>> response_headers;
    int status_code{0};

    bool headers_ready{false};
    bool is_chunked{false};
    bool is_completed{false};
    bool is_cancelled{false};
    bool too_large{false};
    bool total_timeout_expired{false};
    bool eof_delivered{false};

    bool has_total_deadline{false};
    std::chrono::steady_clock::time_point total_deadline{};
    std::size_t accumulated_bytes{0};
    std::size_t effective_limit{0};

    std::deque<std::string> chunk_queue;
    std::string non_chunked_body;
    bool non_chunked_body_delivered{false};

    int task_state{0};
    int task_error{0};
    int timeout_reason{0};

    int configured_receive_timeout{-1};
    int configured_watch_timeout{-1};


    static int clamp_rem_ms(long long rem_count) noexcept {
        if (rem_count <= 0) return 1;
        if (rem_count > std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        return static_cast<int>(rem_count);
    }

    static int clamp_to_deadline(int configured, int rem_ms) noexcept {
        if (configured >= 0 && configured < rem_ms) {
            return configured;
        }
        return rem_ms;
    }

    void expire_total_timeout(std::unique_lock<std::mutex> &lk) noexcept {
        total_timeout_expired = true;
        chunk_queue.clear();
        non_chunked_body.clear();
        if (lk.owns_lock()) {
            lk.unlock();
        }
        cond_data.notify_all();
        cond_headers.notify_all();
    }

    void on_extract(WFHttpChunkedTask *t) {
        std::unique_lock<std::mutex> lk(mutex);
        if (is_cancelled || total_timeout_expired) {
            return;
        }
        if (has_total_deadline) {
            auto now = std::chrono::steady_clock::now();
            if (now >= total_deadline) {
                t->set_watch_timeout(1);
                t->set_receive_timeout(1);
                expire_total_timeout(lk);
                return;
            }
            int rem_ms = clamp_rem_ms(std::chrono::ceil<std::chrono::milliseconds>(total_deadline - now).count());
            if (configured_watch_timeout >= 0) {
                t->set_watch_timeout(clamp_to_deadline(configured_watch_timeout, rem_ms));
            } else {
                t->set_watch_timeout(rem_ms);
            }
            t->set_receive_timeout(clamp_to_deadline(configured_receive_timeout, rem_ms));
        }

        if (t->get_chunk() == nullptr) {
            if (headers_ready) {
                return;
            }
            auto *resp = t->get_resp();
            const char *sc = resp->get_status_code();
            status_code = sc ? std::atoi(sc) : 200;
            const char *msg = resp->get_reason_phrase();
            response_reason = msg ? msg : "";

            protocol::HttpHeaderCursor cursor(resp);
            std::string name, val;
            while (cursor.next(name, val)) {
                response_headers.emplace_back(std::move(name), std::move(val));
            }
            is_chunked = resp->is_chunked();

            // Add 64KiB header headroom with saturation for non-chunked responses.
            std::size_t parser_limit = effective_limit;
            if (!is_chunked) {
                if (parser_limit <= std::numeric_limits<std::size_t>::max() - 65536) {
                    parser_limit += 65536;
                } else {
                    parser_limit = std::numeric_limits<std::size_t>::max();
                }
            }
            resp->set_size_limit(parser_limit);
            const char *cl = nullptr;
            for (const auto &[k, v] : response_headers) {
                if (strcasecmp(k.c_str(), "Content-Length") == 0) {
                    cl = v.c_str();
                    break;
                }
            }
            if (cl) {
                char *end = nullptr;
                unsigned long long cl_val = std::strtoull(cl, &end, 10);
                if (end != cl && cl_val > effective_limit) {
                    too_large = true;
                    resp->set_size_limit(0);
                    t->set_receive_timeout(0);
                }
            }

            headers_ready = true;
            lk.unlock();
            cond_headers.notify_all();
            cond_data.notify_all();
        } else {
            const void *chunk_ptr = nullptr;
            size_t chunk_len = 0;
            t->get_chunk()->get_chunk_data(&chunk_ptr, &chunk_len);
            if (chunk_len > 0) {
                if (accumulated_bytes + chunk_len > effective_limit) {
                    too_large = true;
                    t->set_receive_timeout(0);
                    lk.unlock();
                    cond_data.notify_all();
                    return;
                }
                accumulated_bytes += chunk_len;
                chunk_queue.emplace_back(static_cast<const char *>(chunk_ptr), chunk_len);
                lk.unlock();
                cond_data.notify_all();
            }
        }
    }

    void on_callback(WFHttpChunkedTask *t) {
        std::unique_lock<std::mutex> lk(mutex);
        is_completed = true;
        task_state = t->get_state();
        task_error = t->get_error();
        if (t->get_http_task()) {
            timeout_reason = t->get_http_task()->get_timeout_reason();
        }
        request_body.clear();

        if (task_state == WFT_STATE_SYS_ERROR && task_error == EMSGSIZE) {
            too_large = true;
        }

        if (total_timeout_expired || (has_total_deadline && std::chrono::steady_clock::now() >= total_deadline)) {
            expire_total_timeout(lk);
            return;
        }
        if (is_cancelled) {
            chunk_queue.clear();
            non_chunked_body.clear();
            if (lk.owns_lock()) {
                lk.unlock();
            }
            cond_headers.notify_all();
            cond_data.notify_all();
            return;
        }

        auto *resp = t->get_resp();
        if (!headers_ready) {
            if (task_state == WFT_STATE_SUCCESS) {
                const char *sc = resp->get_status_code();
                status_code = sc ? std::atoi(sc) : 200;
                const char *msg = resp->get_reason_phrase();
                response_reason = msg ? msg : "";

                protocol::HttpHeaderCursor cursor(resp);
                std::string name, val;
                while (cursor.next(name, val)) {
                    response_headers.emplace_back(std::move(name), std::move(val));
                }
                is_chunked = resp->is_chunked();

                if (is_chunked) {
                    non_chunked_body = protocol::HttpUtil::decode_chunked_body(resp);
                } else {
                    const void *body_ptr = nullptr;
                    size_t body_len = 0;
                    if (resp->get_parsed_body(&body_ptr, &body_len) && body_len > 0) {
                        non_chunked_body.assign(static_cast<const char *>(body_ptr), body_len);
                    }
                }
                accumulated_bytes += non_chunked_body.size();
                if (accumulated_bytes > effective_limit) {
                    too_large = true;
                }
                headers_ready = true;
            }
        } else {
            if (!is_chunked && task_state == WFT_STATE_SUCCESS) {
                const void *body_ptr = nullptr;
                size_t body_len = 0;
                if (resp->get_parsed_body(&body_ptr, &body_len) && body_len > 0) {
                    non_chunked_body.assign(static_cast<const char *>(body_ptr), body_len);
                    accumulated_bytes += non_chunked_body.size();
                    if (accumulated_bytes > effective_limit) {
                        too_large = true;
                    }
                }
            }
        }

        lk.unlock();
        cond_headers.notify_all();
        cond_data.notify_all();
    }

    void cancel() noexcept {
        std::unique_lock<std::mutex> lk(mutex);
        if (is_cancelled) {
            return;
        }
        is_cancelled = true;
        chunk_queue.clear();
        non_chunked_body.clear();
        lk.unlock();
        cond_data.notify_all();
        cond_headers.notify_all();
    }
};

static void throw_classified_error(const StreamState &st) {
    if (st.is_cancelled) {
        throw HttpClientError(HttpClientError::Code::Aborted, 0, "Request aborted");
    }
    if (st.total_timeout_expired ||
        (st.has_total_deadline && std::chrono::steady_clock::now() >= st.total_deadline)) {
        throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
    }
    if (st.too_large || (st.task_state == WFT_STATE_SYS_ERROR && st.task_error == EMSGSIZE)) {
        throw HttpBodyTooLargeError(HttpClientError::Code::BodyTooLarge, st.task_error, "Response body exceeded size limit");
    }
    if ((st.task_state == WFT_STATE_SYS_ERROR && st.task_error == ETIMEDOUT) ||
        st.timeout_reason != TOR_NOT_TIMEOUT) {
        throw HttpTimeoutError(HttpClientError::Code::Timeout, st.task_error, "Request timed out");
    }
    if (st.task_state == WFT_STATE_DNS_ERROR) {
        throw HttpClientError(HttpClientError::Code::DnsError, st.task_error, "DNS resolution failed");
    }
    if (st.task_state == WFT_STATE_SSL_ERROR) {
        throw HttpClientError(HttpClientError::Code::TlsError, st.task_error, "SSL/TLS handshake failed");
    }
    if (st.task_state == WFT_STATE_SYS_ERROR && st.task_error == ECONNREFUSED) {
        throw HttpClientError(HttpClientError::Code::ConnectError, st.task_error, "Connection refused");
    }
    if (st.task_state == WFT_STATE_ABORTED) {
        throw HttpClientError(HttpClientError::Code::Aborted, st.task_error, "Request aborted");
    }
    if (st.task_state == WFT_STATE_TASK_ERROR) {
        if (st.task_error == WFT_ERR_URI_PARSE_FAILED ||
            st.task_error == WFT_ERR_URI_SCHEME_INVALID ||
            st.task_error == WFT_ERR_URI_PORT_INVALID) {
            throw HttpClientError(HttpClientError::Code::InvalidUrl, st.task_error, "Invalid URL");
        } else if (st.task_error == WFT_ERR_UPSTREAM_UNAVAILABLE) {
            throw HttpClientError(HttpClientError::Code::ConnectError, st.task_error, "Upstream unavailable");
        } else {
            throw HttpClientError(HttpClientError::Code::ProtocolError, st.task_error, "Task protocol error");
        }
    }
    std::string err_desc = st.task_error != 0 ? strerror(st.task_error) : "unknown error";
    throw HttpClientError(HttpClientError::Code::Unknown, st.task_error, "Request failed: " + err_desc);
}



HttpClientResponse::HttpClientResponse(std::shared_ptr<StreamState> state)
    : state_(std::move(state)) {}

HttpClientResponse::HttpClientResponse(HttpClientResponse &&other) noexcept
    : state_(std::move(other.state_)),
      total_read_bytes_(other.total_read_bytes_) {
    other.total_read_bytes_ = 0;
}

HttpClientResponse &HttpClientResponse::operator=(HttpClientResponse &&other) noexcept {
    if (this != &other) {
        if (state_) {
            state_->cancel();
        }
        state_ = std::move(other.state_);
        total_read_bytes_ = other.total_read_bytes_;
        other.total_read_bytes_ = 0;
    }
    return *this;
}

HttpClientResponse::~HttpClientResponse() {
    if (state_) {
        state_->cancel();
    }
}

int HttpClientResponse::status() const noexcept {
    return state_ ? state_->status_code : 0;
}

std::string_view HttpClientResponse::reason() const noexcept {
    return state_ ? std::string_view(state_->response_reason) : std::string_view{};
}

static const std::vector<std::pair<std::string, std::string>> empty_headers;

const std::vector<std::pair<std::string, std::string>> &HttpClientResponse::headers() const noexcept {
    return state_ ? state_->response_headers : empty_headers;
}

bool HttpClientResponse::has_header(std::string_view key) const noexcept {
    if (!state_) return false;
    for (const auto &[k, v] : state_->response_headers) {
        if (k.size() == key.size() && strncasecmp(k.data(), key.data(), key.size()) == 0) {
            return true;
        }
    }
    return false;
}

std::string HttpClientResponse::header(std::string_view key) const {
    if (!state_) return "";
    std::string result;
    bool is_cookie = (key.size() == 6 && strncasecmp(key.data(), "Cookie", 6) == 0);
    for (const auto &[k, v] : state_->response_headers) {
        if (k.size() == key.size() && strncasecmp(k.data(), key.data(), key.size()) == 0) {
            if (result.empty()) {
                result = v;
            } else {
                result.append(is_cookie ? "; " : ", ");
                result.append(v);
            }
        }
    }
    return result;
}

void HttpClientResponse::cancel() noexcept {
    if (state_) {
        state_->cancel();
    }
}

std::function<void()> HttpClientResponse::make_cancel_hook() const {
    return [st = state_]() noexcept {
        if (st) {
            st->cancel();
        }
    };
}

coke::Task<std::optional<std::string>> HttpClientResponse::read_chunk(std::size_t max_total_bytes) {
    if (!state_) {
        throw HttpClientError(HttpClientError::Code::Unknown, "Response uninitialized");
    }

    std::unique_lock<std::mutex> lk(state_->mutex);
    if (max_total_bytes < state_->effective_limit) {
        state_->effective_limit = max_total_bytes;
    }
    while (true) {
        if (state_->eof_delivered) {
            co_return std::nullopt;
        }
        if (state_->is_cancelled) {
            throw HttpClientError(HttpClientError::Code::Aborted, 0, "Response read cancelled");
        }

        if (state_->total_timeout_expired ||
            (state_->has_total_deadline && std::chrono::steady_clock::now() >= state_->total_deadline)) {
            state_->expire_total_timeout(lk);
            throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
        }

        if (state_->too_large) {
            throw HttpBodyTooLargeError(HttpClientError::Code::BodyTooLarge, EMSGSIZE, "Response body exceeded size limit");
        }
        if (!state_->chunk_queue.empty()) {
            std::string chunk = std::move(state_->chunk_queue.front());
            state_->chunk_queue.pop_front();
            if (total_read_bytes_ + chunk.size() > max_total_bytes) {
                lk.unlock();
                state_->cancel();
                throw HttpBodyTooLargeError(HttpClientError::Code::BodyTooLarge, EMSGSIZE, "Response body exceeded size limit");
            }
            total_read_bytes_ += chunk.size();
            co_return std::optional<std::string>(std::move(chunk));
        }
        if (!state_->non_chunked_body.empty() && !state_->non_chunked_body_delivered) {
            state_->non_chunked_body_delivered = true;
            std::string body = std::move(state_->non_chunked_body);
            if (total_read_bytes_ + body.size() > max_total_bytes) {
                lk.unlock();
                state_->cancel();
                throw HttpBodyTooLargeError(HttpClientError::Code::BodyTooLarge, EMSGSIZE, "Response body exceeded size limit");
            }
            total_read_bytes_ += body.size();
            co_return std::optional<std::string>(std::move(body));
        }
        if (state_->is_completed) {
            if (state_->task_state != WFT_STATE_SUCCESS) {
                throw_classified_error(*state_);
            }
            state_->eof_delivered = true;
            co_return std::nullopt;
        }

        if (state_->has_total_deadline) {
            auto now = std::chrono::steady_clock::now();
            if (now >= state_->total_deadline) {
                state_->expire_total_timeout(lk);
                throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
            }
            auto rem = std::chrono::duration_cast<std::chrono::nanoseconds>(state_->total_deadline - now);
            int ret = co_await state_->cond_data.wait_for(lk, rem);
            if (ret == coke::TOP_TIMEOUT || (state_->has_total_deadline && std::chrono::steady_clock::now() >= state_->total_deadline)) {
                state_->expire_total_timeout(lk);
                throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
            }
        } else {
            co_await state_->cond_data.wait(lk);
        }
    }
}

coke::Task<std::string> HttpClientResponse::read_body(std::size_t max_body_bytes) {
    std::string result;
    while (true) {
        auto chunk = co_await read_chunk(max_body_bytes);
        if (!chunk.has_value()) {
            break;
        }
        if (result.size() + chunk->size() > max_body_bytes) {
            cancel();
            throw HttpBodyTooLargeError(HttpClientError::Code::BodyTooLarge, EMSGSIZE, "Response body exceeded size limit");
        }
        result.append(*chunk);
    }
    co_return result;
}

coke::Task<std::string> HttpClientResponse::read_body() {
    std::size_t limit = state_ ? state_->effective_limit : static_cast<std::size_t>(16 * 1024 * 1024);
    co_return co_await read_body(limit);
}


HttpClientRequest::HttpClientRequest(std::string method, std::string url, HttpClientOptions options)
    : method_(std::move(method)),
      url_(std::move(url)),
      options_(std::move(options)),
      connect_timeout_(options_.connect_timeout),
      send_timeout_(options_.send_timeout),
      receive_timeout_(options_.receive_timeout),
      total_timeout_(options_.total_timeout),
      max_body_bytes_(options_.default_max_body_bytes),
      max_redirects_(options_.max_redirects),
      max_retries_(options_.max_retries) {}

HttpClientRequest &HttpClientRequest::header(std::string key, std::string value) {
    headers_.emplace_back(std::move(key), std::move(value));
    return *this;
}

HttpClientRequest &HttpClientRequest::headers(std::vector<std::pair<std::string, std::string>> hdrs) {
    for (auto &pair : hdrs) {
        headers_.push_back(std::move(pair));
    }
    return *this;
}

HttpClientRequest &HttpClientRequest::body(std::string data) {
    body_ = std::move(data);
    return *this;
}

HttpClientRequest &HttpClientRequest::body(std::string data, std::string content_type) {
    body_ = std::move(data);
    for (auto &pair : headers_) {
        if (strcasecmp(pair.first.c_str(), "Content-Type") == 0) {
            pair.second = std::move(content_type);
            return *this;
        }
    }
    headers_.emplace_back("Content-Type", std::move(content_type));
    return *this;
}

HttpClientRequest &HttpClientRequest::json(const Json &j) {
    return body(j.dump(), "application/json; charset=utf-8");
}

HttpClientRequest &HttpClientRequest::json(std::string_view raw_json) {
    return body(std::string(raw_json), "application/json; charset=utf-8");
}

coke::Task<HttpClientResponse> HttpClientRequest::send() {
    ParsedURI parsed;
    if (URIParser::parse(url_, parsed) != 0 || !parsed.scheme || !parsed.host ||
        (strcasecmp(parsed.scheme, "http") != 0 && strcasecmp(parsed.scheme, "https") != 0)) {
        throw HttpClientError(HttpClientError::Code::InvalidUrl, 0, "Invalid URL: " + url_);
    }
    if (parsed.port && parsed.port[0]) {
        char *end = nullptr;
        long port_val = std::strtol(parsed.port, &end, 10);
        if (*end != '\0' || port_val <= 0 || port_val > 65535) {
            throw HttpClientError(HttpClientError::Code::InvalidUrl, WFT_ERR_URI_PORT_INVALID, "Invalid URL port: " + std::string(parsed.port));
        }
    }

    ParsedURI proxy_parsed;
    bool has_proxy = !options_.proxy_url.empty();
    if (has_proxy) {
        if (URIParser::parse(options_.proxy_url, proxy_parsed) != 0 || !proxy_parsed.scheme || !proxy_parsed.host ||
            (strcasecmp(proxy_parsed.scheme, "http") != 0 && strcasecmp(proxy_parsed.scheme, "https") != 0)) {
            throw HttpClientError(HttpClientError::Code::InvalidUrl, 0, "Invalid proxy URL: " + options_.proxy_url);
        }
        if (proxy_parsed.port && proxy_parsed.port[0]) {
            char *end = nullptr;
            long port_val = std::strtol(proxy_parsed.port, &end, 10);
            if (*end != '\0' || port_val <= 0 || port_val > 65535) {
                throw HttpClientError(HttpClientError::Code::InvalidUrl, WFT_ERR_URI_PORT_INVALID, "Invalid proxy URL port: " + std::string(proxy_parsed.port));
            }
        }
        if (strcasecmp(parsed.scheme, "https") == 0) {
            throw HttpClientError(HttpClientError::Code::ProtocolError, 0, "HTTPS forward proxy tunneling is unsupported by chunked client");
        }
        if (strcasecmp(proxy_parsed.scheme, "https") == 0) {
            throw HttpClientError(HttpClientError::Code::ProtocolError, 0, "HTTPS proxy connection is unsupported by chunked client");
        }
        if (max_redirects_ > 0) {
            throw HttpClientError(HttpClientError::Code::ProtocolError, 0,
                                  "Redirect following through proxy_url is unsupported by chunked client");
        }
    }

    std::string target_host = parsed.host;
    if (parsed.port && parsed.port[0]) {
        bool is_ssl = (strcasecmp(parsed.scheme, "https") == 0);
        int port_val = std::atoi(parsed.port);
        if ((is_ssl && port_val != 443) || (!is_ssl && port_val != 80)) {
            target_host += ":";
            target_host += parsed.port;
        }
    }

    std::string task_url;
    std::string absolute_target_url;
    if (!has_proxy) {
        task_url = url_;
    } else {
        task_url = std::string("http://") + proxy_parsed.host;
        if (proxy_parsed.port && proxy_parsed.port[0]) {
            task_url += ":";
            task_url += proxy_parsed.port;
        }
        task_url += "/";

        absolute_target_url = std::string("http://") + target_host;
        std::string subpath = (parsed.path && parsed.path[0]) ? parsed.path : "/";
        if (subpath.front() != '/') {
            subpath = "/" + subpath;
        }
        absolute_target_url += subpath;
        if (parsed.query && parsed.query[0]) {
            absolute_target_url += "?";
            absolute_target_url += parsed.query;
        }
    }

    bool is_idempotent = (method_ == "GET" || method_ == "HEAD" || method_ == "OPTIONS" ||
                          method_ == "PUT" || method_ == "DELETE");
    int effective_retries = is_idempotent ? max_retries_ : 0;

    bool has_total_deadline = (total_timeout_.count() > 0);
    std::chrono::steady_clock::time_point total_deadline{};
    if (has_total_deadline) {
        total_deadline = std::chrono::steady_clock::now() + total_timeout_;
    }

    for (int attempt = 0; attempt <= effective_retries; ++attempt) {
        if (has_total_deadline && std::chrono::steady_clock::now() >= total_deadline) {
            throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
        }

        auto state = std::make_shared<StreamState>();
        state->request_body = body_;
        state->effective_limit = max_body_bytes_;
        state->has_total_deadline = has_total_deadline;
        state->total_deadline = total_deadline;

        WFHttpChunkedTask *chunked_task = WFHttpChunkedClient::create_chunked_task(
            task_url,
            max_redirects_,
            [state](WFHttpChunkedTask *t) { state->on_extract(t); },
            [state](WFHttpChunkedTask *t) { state->on_callback(t); }
        );

        if (!chunked_task) {
            throw HttpClientError(HttpClientError::Code::InvalidUrl, 0, "Failed to create chunked task for URL: " + task_url);
        }


        chunked_task->extract_on_header(true);

        // Pre-configure parser limit with header headroom for socket-level bound enforcement.
        std::size_t initial_limit = max_body_bytes_;
        if (initial_limit <= std::numeric_limits<std::size_t>::max() - 65536) {
            initial_limit += 65536;
        } else {
            initial_limit = std::numeric_limits<std::size_t>::max();
        }
        chunked_task->get_resp()->set_size_limit(initial_limit);

        state->configured_receive_timeout = receive_timeout_.count() >= 0 ? static_cast<int>(receive_timeout_.count()) : -1;
        state->configured_watch_timeout = state->configured_receive_timeout;

        if (has_total_deadline) {
            auto now = std::chrono::steady_clock::now();
            int rem_ms = StreamState::clamp_rem_ms(std::chrono::ceil<std::chrono::milliseconds>(total_deadline - now).count());
            int cfg_send = send_timeout_.count() >= 0 ? static_cast<int>(send_timeout_.count()) : -1;
            chunked_task->set_send_timeout(StreamState::clamp_to_deadline(cfg_send, rem_ms));
            chunked_task->set_receive_timeout(StreamState::clamp_to_deadline(state->configured_receive_timeout, rem_ms));
            if (state->configured_watch_timeout >= 0) {
                chunked_task->set_watch_timeout(StreamState::clamp_to_deadline(state->configured_watch_timeout, rem_ms));
            } else {
                chunked_task->set_watch_timeout(rem_ms);
            }
        } else {
            if (send_timeout_.count() >= 0) {
                chunked_task->set_send_timeout(static_cast<int>(send_timeout_.count()));
            }
            if (state->configured_receive_timeout >= 0) {
                chunked_task->set_receive_timeout(state->configured_receive_timeout);
                chunked_task->set_watch_timeout(state->configured_receive_timeout);
            }
        }
        if (options_.keep_alive_timeout.count() >= 0) {
            chunked_task->set_keep_alive(static_cast<int>(options_.keep_alive_timeout.count()));
        }

        auto *req = chunked_task->get_req();
        req->set_method(method_);
        if (has_proxy) {
            req->set_request_uri(absolute_target_url.c_str());
        }
        bool has_host = false;
        bool has_proxy_auth = false;
        for (const auto &[k, v] : headers_) {
            if (strcasecmp(k.c_str(), "Host") == 0) {
                has_host = true;
                req->set_header_pair(k, v);
            } else {
                if (strcasecmp(k.c_str(), "Proxy-Authorization") == 0) {
                    has_proxy_auth = true;
                }
                req->add_header_pair(k, v);
            }
        }
        if (has_proxy && !has_host) {
            req->set_header_pair("Host", target_host);
        }
        if (has_proxy && !has_proxy_auth && proxy_parsed.userinfo && proxy_parsed.userinfo[0]) {
            std::string userinfo(proxy_parsed.userinfo);
            StringUtil::url_decode(userinfo);
            std::string b64 = Base64::encode(userinfo);
            req->add_header_pair("Proxy-Authorization", "Basic " + b64);
        }
        if (!state->request_body.empty()) {
            req->append_output_body_nocopy(state->request_body.data(), state->request_body.size());
        }

        chunked_task->start();
        std::unique_lock<std::mutex> lk(state->mutex);
        if (has_total_deadline) {
            while (!state->headers_ready && !state->is_completed && !state->is_cancelled) {
                auto now = std::chrono::steady_clock::now();
                if (now >= total_deadline) {
                    state->expire_total_timeout(lk);
                    throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
                }
                auto rem = std::chrono::duration_cast<std::chrono::nanoseconds>(total_deadline - now);
                int ret = co_await state->cond_headers.wait_for(lk, rem);
                if ((ret == coke::TOP_TIMEOUT || std::chrono::steady_clock::now() >= total_deadline) &&
                    !state->headers_ready && !state->is_completed) {
                    state->expire_total_timeout(lk);
                    throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
                }
            }
        } else {
            while (!state->headers_ready && !state->is_completed && !state->is_cancelled) {
                co_await state->cond_headers.wait(lk);
            }
        }

        if (state->headers_ready && !state->is_cancelled) {
            if (has_total_deadline && std::chrono::steady_clock::now() >= total_deadline) {
                state->expire_total_timeout(lk);
                throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
            }
            HttpClientResponse resp(state);
            co_return resp;
        }

        if (attempt < effective_retries && !state->is_cancelled) {
            if (has_total_deadline && std::chrono::steady_clock::now() >= total_deadline) {
                state->expire_total_timeout(lk);
                throw HttpTimeoutError(HttpClientError::Code::Timeout, ETIMEDOUT, "Request timed out (total timeout exceeded)");
            }
            if (!state->too_large && state->task_error != WFT_ERR_URI_PARSE_FAILED &&
                state->task_error != WFT_ERR_URI_SCHEME_INVALID &&
                state->task_error != WFT_ERR_URI_PORT_INVALID) {
                lk.unlock();
                co_await coke::sleep(std::chrono::milliseconds(10));
                continue;
            }
        }

        lk.unlock();
        throw_classified_error(*state);
    }
    throw HttpClientError(HttpClientError::Code::Unknown, "Retry exhausted without response");
}


HttpClientRequest HttpClient::request(HttpMethod method, std::string url) {
    std::string method_str(method_to_string(method));
    return HttpClientRequest(std::move(method_str), std::move(url), options_);
}

HttpClientRequest HttpClient::request(std::string method, std::string url) {
    return HttpClientRequest(std::move(method), std::move(url), options_);
}

} // namespace kale
