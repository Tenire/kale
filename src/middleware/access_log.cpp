#include "kale/middleware/access_log.h"

#include <chrono>
#include <stdexcept>
#include <utility>

#include "kale/core/http_error.h"
#include "kale/middleware/request_id.h"

namespace kale {
namespace {

void deliver(const AccessLogSink &sink, const Context &ctx,
             std::chrono::steady_clock::time_point start, int status, bool exception) noexcept {
    // Best-effort: errors must not fail the request or replace downstream exceptions.
    const auto elapsed = std::chrono::steady_clock::now() - start;
    try {
        AccessLogRecord record;
        record.method = ctx.method_string();
        record.target = ctx.raw_uri();
        record.route_pattern = ctx.route_pattern();
        record.status_code = status;
        record.duration = std::chrono::duration_cast<std::chrono::microseconds>(elapsed);
        record.request_id = get_request_id(ctx);
        record.downstream_exception = exception;
        record.response_committed = ctx.is_replied() || ctx.is_stream();
        sink(std::move(record));
    } catch (...) {
    }
}

} // namespace

Middleware access_log(AccessLogSink sink) {
    if (!sink) throw std::invalid_argument("access_log requires a sink");
    return [sink = std::move(sink)](Context &ctx, Next next) -> coke::Task<void> {
        const auto start = std::chrono::steady_clock::now();
        try {
            co_await next();
        } catch (const HttpError &error) {
            const int status = (ctx.is_replied() || ctx.is_stream()) ? ctx.status_code() : error.status_code();
            deliver(sink, ctx, start, status, true);
            throw;
        } catch (...) {
            const int status = (ctx.is_replied() || ctx.is_stream()) ? ctx.status_code() : 500;
            deliver(sink, ctx, start, status, true);
            throw;
        }
        deliver(sink, ctx, start, ctx.status_code(), false);
    };
}

} // namespace kale
