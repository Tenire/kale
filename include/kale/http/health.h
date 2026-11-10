#ifndef KALE_HEALTH_H
#define KALE_HEALTH_H

#include <atomic>
#include <functional>

#include "kale/core/router.h"

namespace kale {
class ReadinessState {
public:
    explicit ReadinessState(bool ready = false) noexcept : ready_(ready) {}
    void set_ready(bool ready) noexcept { ready_.store(ready, std::memory_order_release); }
    bool is_ready() const noexcept { return ready_.load(std::memory_order_acquire); }
private:
    std::atomic<bool> ready_;
};

Handler health_handler();
// Readiness handler invoking a callback. Returns 503 if empty, returning false, or on exception.
Handler readiness_handler(std::function<bool()> ready);
// Borrows state, which must outlive handler copies and in-flight requests.
Handler readiness_handler(const ReadinessState &state);
} // namespace kale

#endif // KALE_HEALTH_H
