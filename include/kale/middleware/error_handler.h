#ifndef KALE_MIDDLEWARE_ERROR_HANDLER_H
#define KALE_MIDDLEWARE_ERROR_HANDLER_H

#include "kale/core/context.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"
#include "kale/core/router.h"

namespace kale {

class ErrorFormatters {
public:
    static ErrorFormatter plain_text() {
        return plain_text_error_formatter;
    }
    static ErrorFormatter standard_json() {
        return default_standard_json_formatter();
    }
    static ErrorFormatter problem_details() {
        return default_problem_details_formatter();
    }
    static ErrorFormatter rfc7807() {
        return problem_details();
    }
};

/**
 * @brief Centralized error handler middleware.
 * Catches unhandled downstream exceptions (HttpError, std::exception, and non-std exceptions),
 * formats the response using the configured formatter and policy, and avoids leaking sensitive details
 * in production mode.
 */
Middleware error_handler(ErrorFormatter formatter = ErrorFormatters::standard_json(),
                         ErrorPolicy policy = {});

} // namespace kale

#endif // KALE_MIDDLEWARE_ERROR_HANDLER_H
