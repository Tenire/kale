#include "kale/middleware/rate_limiter.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "kale/core/context.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"
namespace kale {
namespace {
std::chrono::seconds retry_seconds(RateLimitClock::duration duration) {
    return std::max(std::chrono::seconds(1), std::chrono::ceil<std::chrono::seconds>(duration));
}
} // namespace

class MemoryRateLimitStore::Impl {
public:
    explicit Impl(RateLimitPolicy value) : policy(value) {
        if (policy.limit == 0 || policy.window <= RateLimitClock::duration::zero() ||
            policy.max_keys == 0 || policy.max_key_bytes == 0 ||
            policy.max_keys > entries.max_size() || policy.max_key_bytes > std::string().max_size()) {
            throw std::invalid_argument("invalid fixed-window rate limit policy");
        }
    }
    using Expirations = std::multimap<RateLimitClock::time_point, std::string_view>;
    struct Entry {
        std::uint64_t used;
        RateLimitClock::time_point expires;
        Expirations::iterator expiration;
    };
    const RateLimitPolicy policy;
    std::mutex mutex;
    std::map<std::string, Entry, std::less<>> entries;
    Expirations expirations;
    RateLimitClock::time_point latest{};
    bool sampled{false};
};

MemoryRateLimitStore::MemoryRateLimitStore(RateLimitPolicy policy)
    : impl_(std::make_unique<Impl>(policy)) {}
MemoryRateLimitStore::~MemoryRateLimitStore() = default;

RateLimitVerdict MemoryRateLimitStore::consume(std::string_view key, RateLimitClock::time_point now) {
    auto &state = *impl_;
    const std::lock_guard lock(state.mutex);
    if (state.sampled && now < state.latest) now = state.latest;
    state.latest = now;
    state.sampled = true;

    const auto deny_window = [&] {
        return RateLimitVerdict{false, 0, retry_seconds(state.policy.window)};
    };
    if (key.size() > state.policy.max_key_bytes) return deny_window();

    // Bounded maintenance, with one expiry record per live window, never per hit.
    for (std::size_t reclaimed = 0; reclaimed < 64 && !state.expirations.empty(); ++reclaimed) {
        auto expiration = state.expirations.begin();
        if (expiration->first > now) break;
        auto entry = state.entries.find(expiration->second);
        state.expirations.erase(expiration);
        if (entry != state.entries.end()) state.entries.erase(entry);
    }

    auto entry = state.entries.find(key);
    if (entry != state.entries.end() && entry->second.expires <= now) {
        state.expirations.erase(entry->second.expiration);
        state.entries.erase(entry);
        entry = state.entries.end();
    }
    if (entry != state.entries.end()) {
        auto &window = entry->second;
        if (window.used >= state.policy.limit) {
            const auto wait_time = window.expires > now ? window.expires - now : RateLimitClock::duration::zero();
            return {false, 0, retry_seconds(wait_time)};
        }
        ++window.used;
        return {true, state.policy.limit - window.used, std::chrono::seconds::zero()};
    }
    if (state.entries.size() >= state.policy.max_keys) {
        const auto earliest = state.expirations.begin()->first;
        const auto wait_time = earliest > now ? earliest - now : RateLimitClock::duration::zero();
        return {false, 0, retry_seconds(wait_time)};
    }
    // Avoid signed duration overflow when scheduling a new window near time_point::max().
    if (now.time_since_epoch() > RateLimitClock::duration::max() - state.policy.window) {
        return deny_window();
    }
    const auto expires = now + state.policy.window;
    const auto inserted = state.entries.emplace(
        std::string(key), Impl::Entry{1, expires, state.expirations.end()}).first;
    try {
        inserted->second.expiration = state.expirations.emplace(expires, inserted->first);
    } catch (...) {
        state.entries.erase(inserted);
        throw;
    }
    return {true, state.policy.limit - 1, std::chrono::seconds::zero()};
}

std::size_t MemoryRateLimitStore::size() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->entries.size();
}

Middleware rate_limiter(RateLimitStore &store, RateLimiterConfig config) {
    return [&store, config = std::move(config)](Context &ctx, Next next) -> coke::Task<void> {
        std::string key = config.key_extractor ? config.key_extractor(ctx) : ctx.peer_addr();
        if (!config.route_tag.empty()) {
            // Length-delimit the base key so partition values cannot collide.
            key = std::to_string(key.size()) + ":" + key + std::string(ctx.route_tag(config.route_tag));
        }
        const auto now = config.now ? config.now() : RateLimitClock::now();
        const auto verdict = store.consume(key, now);
        if (!verdict.allowed) {
            const int retry_after_sec = static_cast<int>(
                std::max(std::chrono::seconds(1), verdict.retry_after).count());
            reply_error(ctx, HttpError::too_many_requests("Too Many Requests", retry_after_sec));
            co_return;
        }
        co_await next();
    };
}
} // namespace kale
