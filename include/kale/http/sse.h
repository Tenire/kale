#ifndef KALE_SSE_H
#define KALE_SSE_H
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "coke/queue.h"
#include "coke/task.h"

namespace kale {

class Context;
/**
 * @brief Exception thrown when attempting to send on a closed or disconnected SSE stream.
 */
class DeadStreamLoopException : public std::runtime_error {
public:
    explicit DeadStreamLoopException(std::string path, size_t send_count = 0)
        : std::runtime_error("Cannot send on a closed SSE stream: " + path),
          path_(std::move(path)),
          send_count_(send_count) {}

    const std::string &path() const noexcept { return path_; }
    size_t send_count() const noexcept { return send_count_; }

private:
    std::string path_;
    size_t send_count_{0};
};

struct SseEvent {
    std::string id;
    std::string event;
    std::string data;
    int retry{-1};
    std::string comment;

    std::string format() const;
};

class SseEmitter {
public:
    explicit SseEmitter(Context &ctx);
    ~SseEmitter();

    SseEmitter(const SseEmitter &) = delete;
    SseEmitter &operator=(const SseEmitter &) = delete;
    SseEmitter(SseEmitter &&other) noexcept;
    SseEmitter &operator=(SseEmitter &&other) noexcept;

    // Format inputs immediately; the returned task owns the formatted payload.
    coke::Task<bool> send(std::string_view data);
    coke::Task<bool> send(std::string_view event, std::string_view data);
    coke::Task<bool> send_event(const SseEvent &ev);
    coke::Task<bool> send_comment(std::string_view comment);
    coke::Task<bool> send_chunk(std::string_view raw_data);
    coke::Task<bool> send_chunk(std::string raw_data);
    coke::Task<bool> send_chunk(const char *raw_data) {
        return send_chunk(std::string_view(raw_data ? raw_data : ""));
    }

    /**
     * @brief Consumes string payloads until the queue is closed or transport terminates.
     *
     * Commits stream headers immediately. Transport disconnect closes the supplied queue
     * (which must be dedicated to this subscriber) and terminates without drain; application
     * queue close drains remaining items.
     */
    coke::Task<void> consume(std::shared_ptr<coke::Queue<std::string>> queue);

    /**
     * @brief Consumes structured SseEvent items from a dedicated per-subscriber queue.
     */
    coke::Task<void> consume(std::shared_ptr<coke::Queue<SseEvent>> queue);
    bool is_connected() const noexcept;
    coke::Task<void> close();
    bool is_closed() const noexcept { return closed_; }

private:
    bool ensure_sse_configured();
    void clear_stream_close_hook();
    template <typename T>
    struct ConsumeScopeGuard;
    template <typename T, typename SendFn>
    coke::Task<void> consume_impl(std::shared_ptr<coke::Queue<T>> queue, SendFn send_fn);


    Context *ctx_{nullptr}; // Borrowed
    size_t send_count_{0};
    bool configured_{false};
    bool closed_{false};
};

} // namespace kale

#endif // KALE_SSE_H
