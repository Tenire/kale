#ifndef KALE_CORE_HTTP_ERROR_H
#define KALE_CORE_HTTP_ERROR_H

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kale/core/http_status.h"

namespace kale {

class HttpError : public std::runtime_error {
public:
    using HeaderList = std::vector<std::pair<std::string, std::string>>;
    using DetailMap  = std::map<std::string, std::string, std::less<>>;

    HttpError(int status, std::string message, std::string error_code = "", bool is_exposed = true);
    HttpError(HttpStatus status, std::string message, std::string error_code = "", bool is_exposed = true);

    virtual ~HttpError() = default;

    int status_code() const noexcept { return status_; }
    std::string_view error_code() const noexcept { return error_code_; }
    bool is_exposed() const noexcept { return is_exposed_; }

    HttpError &set_exposed(bool exposed) noexcept {
        is_exposed_ = exposed;
        return *this;
    }

    HttpError &with_code(std::string code) {
        error_code_ = std::move(code);
        return *this;
    }

    HttpError &with_header(std::string key, std::string value);
    HttpError &with_detail(std::string key, std::string value);

    const HeaderList &headers() const noexcept { return headers_; }
    const DetailMap &details() const noexcept { return details_; }

    // RFC semantic factories
    static HttpError bad_request(std::string message = "Bad Request", std::string error_code = "BAD_REQUEST");
    static HttpError unauthorized(std::string message = "Unauthorized", std::string error_code = "UNAUTHORIZED", std::string_view realm = "");
    static HttpError forbidden(std::string message = "Forbidden", std::string error_code = "FORBIDDEN");
    static HttpError not_found(std::string message = "Not Found", std::string error_code = "NOT_FOUND");
    static HttpError method_not_allowed(std::string allow_header = "", std::string message = "Method Not Allowed", std::string error_code = "METHOD_NOT_ALLOWED");
    static HttpError conflict(std::string message = "Conflict", std::string error_code = "CONFLICT");
    static HttpError payload_too_large(std::string message = "Payload Too Large", std::string error_code = "PAYLOAD_TOO_LARGE");
    static HttpError unprocessable_entity(std::string message = "Unprocessable Entity", std::string error_code = "VALIDATION_FAILED");
    static HttpError too_many_requests(std::string message = "Too Many Requests", int retry_after_seconds = 0, std::string error_code = "RATE_LIMITED");
    static HttpError internal_error(std::string message = "Internal Server Error", std::string error_code = "INTERNAL_SERVER_ERROR", bool is_exposed = false);
    static HttpError service_unavailable(std::string message = "Service Unavailable", int retry_after_seconds = 0, std::string error_code = "SERVICE_UNAVAILABLE");

private:
    int status_{static_cast<int>(HttpStatus::InternalServerError)};
    std::string error_code_;
    bool is_exposed_{true};
    HeaderList headers_;
    DetailMap details_;
};

class JsonInputError : public HttpError {
public:
    explicit JsonInputError(std::string message = "invalid JSON")
        : HttpError(static_cast<int>(HttpStatus::BadRequest), std::move(message), "INVALID_JSON", true) {}
};

} // namespace kale

#endif // KALE_CORE_HTTP_ERROR_H
