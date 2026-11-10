#include "kale/http/health.h"

#include <stdexcept>
#include <utility>

#include "kale/core/context.h"

namespace kale {
Handler health_handler() {
    return make_handler([](Context &ctx) {
        ctx.set_status(200).string("OK");
    });
}

Handler readiness_handler(std::function<bool()> ready) {
    if (!ready) throw std::invalid_argument("readiness_handler requires a callback");
    return make_handler([ready = std::move(ready)](Context &ctx) {
        bool available = false;
        try {
            available = ready();
        } catch (...) {
        }
        ctx.set_status(available ? 200 : 503).string(available ? "OK" : "Not Ready");
    });
}

Handler readiness_handler(const ReadinessState &state) {
    return readiness_handler([&state] { return state.is_ready(); });
}
} // namespace kale
