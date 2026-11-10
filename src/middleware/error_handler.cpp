#include "kale/middleware/error_handler.h"

namespace kale {

Middleware error_handler(ErrorFormatter formatter, ErrorPolicy policy) {
    return [formatter = std::move(formatter), policy = std::move(policy)](Context &ctx, Next next) -> coke::Task<void> {
        const auto *prev_formatter = ctx.error_formatter();
        const auto *prev_policy = ctx.error_policy();
        ctx.set_error_config(&formatter, &policy);

        std::exception_ptr ep;
        try {
            co_await next();
        } catch (...) {
            ep = std::current_exception();
        }

        ctx.set_error_config(prev_formatter, prev_policy);

        if (!ep) {
            co_return;
        }

        if (ctx.is_stream() || ctx.is_replied()) {
            std::rethrow_exception(ep);
        }

        reply_error(ctx, ep, formatter, policy);
        co_return;
    };
}

} // namespace kale
