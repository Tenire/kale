#include "kale/http/sse.h"

#include <string>
#include <string_view>
#include "core/stream_access.h"
#include "kale/core/context.h"

namespace kale {

namespace {

static void append_prefixed_lines(std::string &out, std::string_view prefix, std::string_view content) {
    size_t start = 0;
    while (start <= content.size()) {
        size_t end = content.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
            out.append(prefix).append(content.substr(start)).append("\n");
            break;
        }
        out.append(prefix).append(content.substr(start, end - start)).append("\n");
        if (content[end] == '\r' && end + 1 < content.size() && content[end + 1] == '\n') {
            start = end + 2;
        } else {
            start = end + 1;
        }
    }
}

static std::string sanitize_single_line(std::string_view str) {
    std::string res;
    res.reserve(str.size());
    for (char c : str) {
        if (c != '\r' && c != '\n') {
            res.push_back(c);
        }
    }
    return res;
}

std::string format_sse_fields(std::string_view id,
                              std::string_view event,
                              std::string_view data,
                              int retry,
                              std::string_view comment,
                              bool force_data_field) {
    std::string out;
    out.reserve(comment.size() + id.size() + event.size() + data.size() + 32);

    if (!comment.empty()) {
        append_prefixed_lines(out, ": ", comment);
    }

    if (retry >= 0) {
        out.append("retry: ").append(std::to_string(retry)).append("\n");
    }

    if (!id.empty()) {
        out.append("id: ").append(sanitize_single_line(id)).append("\n");
    }

    if (!event.empty()) {
        out.append("event: ").append(sanitize_single_line(event)).append("\n");
    }

    if (!data.empty()) {
        append_prefixed_lines(out, "data: ", data);
    } else if (force_data_field || !id.empty() || !event.empty() || retry >= 0) {
        out.append("data:\n");
    }
    out.append("\n");
    return out;
}

} // namespace

std::string SseEvent::format() const {
    return format_sse_fields(id, event, data, retry, comment, false);
}

SseEmitter::SseEmitter(Context &ctx)
    : ctx_(&ctx) {
}

SseEmitter::~SseEmitter() = default;

SseEmitter::SseEmitter(SseEmitter &&other) noexcept
    : ctx_(other.ctx_),
      send_count_(other.send_count_),
      configured_(other.configured_),
      closed_(other.closed_) {
    other.ctx_ = nullptr;
    other.closed_ = true;
}

SseEmitter &SseEmitter::operator=(SseEmitter &&other) noexcept {
    if (this != &other) {
        ctx_ = other.ctx_;
        send_count_ = other.send_count_;
        configured_ = other.configured_;
        closed_ = other.closed_;
        other.ctx_ = nullptr;
        other.closed_ = true;
    }
    return *this;
}

bool SseEmitter::ensure_sse_configured() {
    if (configured_) {
        return true;
    }
    if (!ctx_ || closed_) {
        closed_ = true;
        return false;
    }

    ctx_->set_header("Content-Type", "text/event-stream");
    ctx_->set_header("Cache-Control", "no-cache");
    configured_ = true;
    return true;
}

coke::Task<bool> SseEmitter::send(std::string_view data) {
    return send_chunk(format_sse_fields("", "", data, -1, "", true));
}

coke::Task<bool> SseEmitter::send(std::string_view event, std::string_view data) {
    return send_chunk(format_sse_fields("", event, data, -1, "", true));
}

coke::Task<bool> SseEmitter::send_event(const SseEvent &ev) {
    return send_chunk(ev.format());
}

coke::Task<bool> SseEmitter::send_comment(std::string_view comment) {
    std::string text = comment.empty() ? ":\n\n" : format_sse_fields("", "", "", -1, comment, false);
    return send_chunk(std::move(text));
}

coke::Task<bool> SseEmitter::send_chunk(std::string_view raw_data) {
    if (closed_ || !ctx_) {
        throw DeadStreamLoopException(std::string(ctx_ ? ctx_->path() : ""), send_count_);
    }
    if (!ensure_sse_configured()) {
        closed_ = true;
        co_return false;
    }
    bool ok = co_await ctx_->send_chunk(raw_data);
    if (!ok) {
        closed_ = true;
        co_return false;
    }
    ++send_count_;
    co_return true;
}
coke::Task<bool> SseEmitter::send_chunk(std::string raw_data) {
    if (closed_ || !ctx_) {
        throw DeadStreamLoopException(std::string(ctx_ ? ctx_->path() : ""), send_count_);
    }
    if (!ensure_sse_configured()) {
        closed_ = true;
        co_return false;
    }
    bool ok = co_await ctx_->send_chunk(std::move(raw_data));
    if (!ok) {
        closed_ = true;
        co_return false;
    }
    ++send_count_;
    co_return true;
}

bool SseEmitter::is_connected() const noexcept {
    if (closed_ || !ctx_) {
        return false;
    }
    return ctx_->is_connected();
}

coke::Task<void> SseEmitter::close() {
    if (closed_ || !ctx_) {
        co_return;
    }
    closed_ = true;
    co_await ctx_->end_chunk();
}
void SseEmitter::clear_stream_close_hook() {
    if (ctx_) {
        StreamAccess::clear_close_hook(*ctx_);
    }
}

template <typename T>
struct SseEmitter::ConsumeScopeGuard {
    SseEmitter *emitter{nullptr};
    std::shared_ptr<coke::Queue<T>> queue;
    bool completed_normally{false};

    ~ConsumeScopeGuard() {
        if (emitter) {
            emitter->clear_stream_close_hook();
        }
        if (!completed_normally && queue) {
            queue->close();
        }
    }
};

template <typename T, typename SendFn>
coke::Task<void> SseEmitter::consume_impl(std::shared_ptr<coke::Queue<T>> queue, SendFn send_fn) {
    ConsumeScopeGuard<T> guard{.emitter = this, .queue = queue, .completed_normally = false};

    if (!ensure_sse_configured()) {
        closed_ = true;
        co_return;
    }
    if (!StreamAccess::start(*ctx_)) {
        closed_ = true;
        co_return;
    }

    auto weak_q = std::weak_ptr<coke::Queue<T>>(queue);
    StreamAccess::set_close_hook(*ctx_, [weak_q]() noexcept {
        if (auto q = weak_q.lock()) {
            q->close();
        }
    });

    while (true) {
        T item;
        int ret = co_await queue->pop(item);
        if (ret != coke::TOP_SUCCESS) {
            break;
        }

        if (!is_connected()) {
            break;
        }

        bool ok = co_await send_fn(item);
        if (!ok) {
            queue->close();
            break;
        }
    }

    if (is_connected()) {
        co_await close();
        guard.completed_normally = true;
    }
}

coke::Task<void> SseEmitter::consume(std::shared_ptr<coke::Queue<std::string>> queue) {
    if (!queue) {
        throw std::invalid_argument("SseEmitter::consume requires a valid queue");
    }
    if (closed_ || !ctx_) {
        queue->close();
        throw DeadStreamLoopException(std::string(ctx_ ? ctx_->path() : ""), send_count_);
    }
    co_await consume_impl(queue, [this](const std::string &item) {
        return send(item);
    });
}

coke::Task<void> SseEmitter::consume(std::shared_ptr<coke::Queue<SseEvent>> queue) {
    if (!queue) {
        throw std::invalid_argument("SseEmitter::consume requires a valid queue");
    }
    if (closed_ || !ctx_) {
        queue->close();
        throw DeadStreamLoopException(std::string(ctx_ ? ctx_->path() : ""), send_count_);
    }
    co_await consume_impl(queue, [this](const SseEvent &item) {
        return send_event(item);
    });
}

} // namespace kale
