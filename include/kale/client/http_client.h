#ifndef KALE_HTTP_CLIENT_H
#define KALE_HTTP_CLIENT_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "coke/task.h"
#include "kale/core/http_method.h"

namespace kale {

class Json;
struct HttpClientOptions {
    // The incremental backend has no per-request TCP connect-timeout control.
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds send_timeout{5000};
    std::chrono::milliseconds receive_timeout{10000};
    std::chrono::milliseconds keep_alive_timeout{60000};
    std::chrono::milliseconds total_timeout{0};
    int max_redirects{0};
    int max_retries{0};
    std::size_t default_max_body_bytes{16 * 1024 * 1024};
    std::string proxy_url{};
};

class HttpClientError : public std::runtime_error {
public:
    enum class Code {
        InvalidUrl,
        DnsError,
        ConnectError,
        TlsError,
        Timeout,
        SendError,
        ReceiveError,
        ProtocolError,
        BodyTooLarge,
        Aborted,
        Unknown
    };

    HttpClientError(Code code, int sys_err, std::string msg)
        : std::runtime_error(std::move(msg)), code_(code), sys_err_(sys_err) {}

    HttpClientError(Code code, std::string msg)
        : std::runtime_error(std::move(msg)), code_(code), sys_err_(0) {}

    Code code() const noexcept { return code_; }
    int sys_error() const noexcept { return sys_err_; }
    bool is_timeout() const noexcept { return code_ == Code::Timeout; }

private:
    Code code_;
    int sys_err_{0};
};

class HttpTimeoutError : public HttpClientError {
public:
    HttpTimeoutError(Code code, int sys_err, std::string msg)
        : HttpClientError(code, sys_err, std::move(msg)) {}
    explicit HttpTimeoutError(std::string msg)
        : HttpClientError(Code::Timeout, 0, std::move(msg)) {}
};

class HttpBodyTooLargeError : public HttpClientError {
public:
    HttpBodyTooLargeError(Code code, int sys_err, std::string msg)
        : HttpClientError(code, sys_err, std::move(msg)) {}
    explicit HttpBodyTooLargeError(std::string msg)
        : HttpClientError(Code::BodyTooLarge, 0, std::move(msg)) {}
};

struct StreamState;
class ProxyStreamBridge;
class HttpClientRequest;

class HttpClientResponse {
    friend class HttpClientRequest;
    friend class ProxyStreamBridge;
public:
    HttpClientResponse() = default;
    explicit HttpClientResponse(std::shared_ptr<StreamState> state);
    ~HttpClientResponse();

    HttpClientResponse(const HttpClientResponse &) = delete;
    HttpClientResponse &operator=(const HttpClientResponse &) = delete;
    HttpClientResponse(HttpClientResponse &&) noexcept;
    HttpClientResponse &operator=(HttpClientResponse &&) noexcept;

    int status() const noexcept;
    std::string_view reason() const noexcept;
    std::string header(std::string_view key) const;
    bool has_header(std::string_view key) const noexcept;
    const std::vector<std::pair<std::string, std::string>> &headers() const noexcept;

    coke::Task<std::string> read_body(std::size_t max_body_bytes);
    coke::Task<std::string> read_body();
    coke::Task<std::optional<std::string>> read_chunk(std::size_t max_total_bytes = static_cast<std::size_t>(-1));
    // Cancels delivery and wakes pending readers; does not guarantee immediate socket termination.
    void cancel() noexcept;

private:
    std::function<void()> make_cancel_hook() const;
    std::shared_ptr<StreamState> state_;
    std::size_t total_read_bytes_{0};
};

class HttpClientRequest {
public:
    HttpClientRequest(std::string method, std::string url, HttpClientOptions options);

    HttpClientRequest &header(std::string key, std::string value);
    HttpClientRequest &headers(std::vector<std::pair<std::string, std::string>> hdrs);
    HttpClientRequest &body(std::string data);
    HttpClientRequest &body(std::string data, std::string content_type);
    HttpClientRequest &json(const Json &j);
    HttpClientRequest &json(std::string_view raw_json);

    template <typename Rep, typename Period>
    HttpClientRequest &timeout(std::chrono::duration<Rep, Period> d) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d);
        connect_timeout_ = ms;
        send_timeout_ = ms;
        receive_timeout_ = ms;
        total_timeout_ = ms;
        return *this;
    }

    // The incremental backend has no per-request TCP connect-timeout control.
    template <typename Rep, typename Period>
    HttpClientRequest &connect_timeout(std::chrono::duration<Rep, Period> d) {
        connect_timeout_ = std::chrono::duration_cast<std::chrono::milliseconds>(d);
        return *this;
    }

    template <typename Rep, typename Period>
    HttpClientRequest &send_timeout(std::chrono::duration<Rep, Period> d) {
        send_timeout_ = std::chrono::duration_cast<std::chrono::milliseconds>(d);
        return *this;
    }

    template <typename Rep, typename Period>
    HttpClientRequest &receive_timeout(std::chrono::duration<Rep, Period> d) {
        receive_timeout_ = std::chrono::duration_cast<std::chrono::milliseconds>(d);
        return *this;
    }

    template <typename Rep, typename Period>
    HttpClientRequest &total_timeout(std::chrono::duration<Rep, Period> d) {
        total_timeout_ = std::chrono::duration_cast<std::chrono::milliseconds>(d);
        return *this;
    }

    HttpClientRequest &max_body_bytes(std::size_t bytes) {
        max_body_bytes_ = bytes;
        return *this;
    }

    HttpClientRequest &follow_redirects(int max_redirects) {
        max_redirects_ = max_redirects;
        return *this;
    }

    HttpClientRequest &retry(int max_retries) {
        max_retries_ = max_retries;
        return *this;
    }
    const std::string &url() const noexcept { return url_; }
    const std::string &method() const noexcept { return method_; }
    const std::vector<std::pair<std::string, std::string>> &headers() const noexcept { return headers_; }
    const std::string &body_data() const noexcept { return body_; }

    coke::Task<HttpClientResponse> send();

private:
    std::string method_;
    std::string url_;
    HttpClientOptions options_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    std::chrono::milliseconds connect_timeout_;
    std::chrono::milliseconds send_timeout_;
    std::chrono::milliseconds receive_timeout_;
    std::chrono::milliseconds total_timeout_;
    std::size_t max_body_bytes_;
    int max_redirects_;
    int max_retries_;
};

class HttpClient {
public:
    explicit HttpClient(HttpClientOptions options = {}) : options_(std::move(options)) {}

    HttpClientRequest get(std::string url) { return request("GET", std::move(url)); }
    HttpClientRequest post(std::string url) { return request("POST", std::move(url)); }
    HttpClientRequest put(std::string url) { return request("PUT", std::move(url)); }
    HttpClientRequest del(std::string url) { return request("DELETE", std::move(url)); }
    HttpClientRequest delete_(std::string url) { return request("DELETE", std::move(url)); }
    HttpClientRequest patch(std::string url) { return request("PATCH", std::move(url)); }
    HttpClientRequest head(std::string url) { return request("HEAD", std::move(url)); }
    HttpClientRequest options(std::string url) { return request("OPTIONS", std::move(url)); }

    HttpClientRequest request(HttpMethod method, std::string url);
    HttpClientRequest request(std::string method, std::string url);

    const HttpClientOptions &options() const noexcept { return options_; }

private:
    HttpClientOptions options_;
};

} // namespace kale

#endif // KALE_HTTP_CLIENT_H
