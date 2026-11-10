#ifndef KALE_MIDDLEWARE_ACCESS_LOG_H
#define KALE_MIDDLEWARE_ACCESS_LOG_H

#include <chrono>
#include <functional>
#include <string>

#include "kale/core/router.h"

namespace kale {

struct AccessLogRecord {
    std::string method;
    std::string target;
    std::string route_pattern; // Empty for unmatched or 405 requests.
    int status_code{0};
    std::chrono::microseconds duration{0};
    std::string request_id; // Populated from RequestId attachment when present.
    bool downstream_exception{false};
    bool response_committed{false};
};

using AccessLogSink = std::function<void(AccessLogRecord)>;

// Throws std::invalid_argument if sink is empty.
// Sinks are invoked synchronously on worker threads and must be thread-safe.
// Exceptions from the sink or record construction are swallowed.
// Install outside error handlers and response transformers to observe them.
// Duration measures downstream middleware execution, not wire delivery.
Middleware access_log(AccessLogSink sink);

} // namespace kale

#endif // KALE_MIDDLEWARE_ACCESS_LOG_H
