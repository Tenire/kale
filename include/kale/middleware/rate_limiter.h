#ifndef KALE_MIDDLEWARE_RATE_LIMITER_H
#define KALE_MIDDLEWARE_RATE_LIMITER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "kale/core/router.h"

namespace kale {
using RateLimitClock = std::chrono::steady_clock;

struct RateLimitPolicy {
    std::uint64_t limit{100};
    RateLimitClock::duration window{std::chrono::seconds(1)};
    std::size_t max_keys{10000};
    std::size_t max_key_bytes{1024};
};

struct RateLimitVerdict {
    bool allowed{false};
    std::uint64_t remaining{0};
    std::chrono::seconds retry_after{0};
};

// Interface for rate limit storage. Implementations must be thread-safe.
class RateLimitStore {
public:
    virtual ~RateLimitStore() = default;
    virtual RateLimitVerdict consume(std::string_view key, RateLimitClock::time_point now) = 0;
};

// In-memory fixed-window rate limit store. Thread-safe.
class MemoryRateLimitStore final : public RateLimitStore {
public:
    explicit MemoryRateLimitStore(RateLimitPolicy policy = {});
    ~MemoryRateLimitStore() override;
    MemoryRateLimitStore(const MemoryRateLimitStore &) = delete;
    MemoryRateLimitStore &operator=(const MemoryRateLimitStore &) = delete;
    RateLimitVerdict consume(std::string_view key, RateLimitClock::time_point now) override;
    std::size_t size() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct RateLimiterConfig {
    // Defaults to client peer IP when not specified.
    std::function<std::string(const Context &)> key_extractor;
    // Optional tag to partition rate limits across route groups.
    std::string route_tag;
    std::function<RateLimitClock::time_point()> now;
};

// Enforces rate limits using store. Denied requests receive 429 with Retry-After.
// store must outlive the middleware and active requests.
Middleware rate_limiter(RateLimitStore &store, RateLimiterConfig config = {});
} // namespace kale

#endif // KALE_MIDDLEWARE_RATE_LIMITER_H
