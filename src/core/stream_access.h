#ifndef KALE_CORE_STREAM_ACCESS_H
#define KALE_CORE_STREAM_ACCESS_H

#include <functional>
#include <utility>

#include "kale/core/context.h"

namespace kale {

class StreamAccess {
public:
    static bool start(Context &ctx) { return ctx.start_stream(); }

    static void set_close_hook(Context &ctx, std::function<void()> hook) {
        ctx.set_stream_close_hook(std::move(hook));
    }

    static void clear_close_hook(Context &ctx) {
        ctx.clear_stream_close_hook();
    }
};

} // namespace kale

#endif // KALE_CORE_STREAM_ACCESS_H
