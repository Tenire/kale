#ifndef KALE_CORE_ERROR_RESPONSE_H
#define KALE_CORE_ERROR_RESPONSE_H

#include <exception>
#include <functional>
#include <string>
#include <string_view>

#include "kale/core/http_error.h"
#include "kale/core/logger.h"

namespace kale {

class Context;

struct ErrorContext {
    int status_code{500};
    std::string_view error_code;
    std::string_view message;
    bool is_exposed{false};
    const HttpError::DetailMap *details{nullptr};
    const HttpError::HeaderList *headers{nullptr};
    const std::exception *raw_exception{nullptr};
    std::exception_ptr exception{};
};

using ErrorFormatter = std::function<void(Context &ctx, const ErrorContext &err_ctx)>;

struct ErrorPolicy {
    bool expose_internal_errors{false};
    bool always_http_200{false};
    int default_status_code{500};
    std::string default_blurred_message{"Internal Server Error"};
    std::string default_error_code{"INTERNAL_SERVER_ERROR"};
    Logger logger{};
};

// Pure-string core formatters (zero high-layer dependencies)
void plain_text_error_formatter(Context &ctx, const ErrorContext &err_ctx);
void standard_json_error_formatter(Context &ctx, const ErrorContext &err_ctx);
void problem_details_error_formatter(Context &ctx, const ErrorContext &err_ctx);

const ErrorFormatter &default_plain_text_formatter() noexcept;
const ErrorFormatter &default_standard_json_formatter() noexcept;
const ErrorFormatter &default_problem_details_formatter() noexcept;
const ErrorPolicy &default_error_policy() noexcept;

void clear_framing_headers(Context &ctx) noexcept;
void apply_error_headers(Context &ctx, const HttpError::HeaderList &headers);

// Direct error writes (borrows active config on Context, or defaults if unset)
void reply_error(Context &ctx, const HttpError &err);
void reply_error(Context &ctx, const HttpError &err, const ErrorFormatter &formatter, const ErrorPolicy &policy);
void reply_error(Context &ctx, std::exception_ptr ep);
void reply_error(Context &ctx, std::exception_ptr ep, const ErrorFormatter &formatter, const ErrorPolicy &policy);

} // namespace kale

#endif // KALE_CORE_ERROR_RESPONSE_H
