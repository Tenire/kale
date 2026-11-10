#ifndef KALE_CORE_STREAM_REPLY_ADAPTER_H
#define KALE_CORE_STREAM_REPLY_ADAPTER_H

#include <atomic>
#include <coroutine>
#include <cerrno>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <sys/uio.h>
#include <utility>
#include <vector>

#include "coke/basic_awaiter.h"
#include "workflow/StreamReply.h"
#include "workflow/WFTaskFactory.h"

namespace kale {

class CloseBridge {
public:
    CloseBridge() noexcept : ref_count_(1) {}

    void add_ref() noexcept {
        ref_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void release() noexcept {
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    void set_hook(std::function<void()> hook) {
        std::function<void()> immediate;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                immediate = std::move(hook);
            } else {
                hook_ = std::move(hook);
            }
        }
        if (immediate) {
            immediate();
        }
    }

    void clear_hook() noexcept {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn = std::move(hook_);
        }
    }

    void notify_closed() noexcept {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return;
            closed_ = true;
            fn = std::move(hook_);
        }
        if (fn) {
            fn();
        }
    }

    bool is_closed() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::mutex mutex_;
    bool closed_{false};
    std::function<void()> hook_;
};

class StreamReplyAdapter {
public:
    explicit StreamReplyAdapter(::StreamReply *stream)
        : stream_(stream) {
        bridge_ = new CloseBridge();
        arm_close_callback();
    }

    ~StreamReplyAdapter() {
        abort();
        if (bridge_) {
            bridge_->release();
            bridge_ = nullptr;
        }
    }

    StreamReplyAdapter(const StreamReplyAdapter &) = delete;
    StreamReplyAdapter &operator=(const StreamReplyAdapter &) = delete;
    StreamReplyAdapter(StreamReplyAdapter &&) = delete;
    StreamReplyAdapter &operator=(StreamReplyAdapter &&) = delete;

    bool is_connected() noexcept {
        if (bridge_ && bridge_->is_closed()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_ || finished_ || !stream_) return false;
        return stream_->connected();
    }

    bool is_aborted() noexcept {
        if (bridge_ && bridge_->is_closed()) return true;
        std::lock_guard<std::mutex> lock(mutex_);
        return aborted_;
    }

    bool is_finished() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

    void set_close_hook(std::function<void()> hook) {
        if (bridge_) {
            bridge_->set_hook(std::move(hook));
        }
    }

    void clear_close_hook() noexcept {
        if (bridge_) {
            bridge_->clear_hook();
        }
    }

    void abort() noexcept {
        std::vector<WFCounterTask *> to_release;
        ::StreamReply *stream_to_abort = nullptr;
        CloseBridge *br = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (aborted_ || finished_) return;
            aborted_ = true;
            br = bridge_;
            if (br) br->add_ref();
            disarm_close_callback_locked();
            stream_to_abort = std::exchange(stream_, nullptr);
            while (!queue_.empty()) {
                auto *op = queue_.front();
                queue_.pop_front();
                op->result = false;
                if (op->gate) {
                    to_release.push_back(op->gate);
                }
            }
        }
        // The current write is NOT released here: native cancellation must retire
        // its polling node before callback_thunk can resume its payload owner.
        // Only never-submitted queued operations can complete immediately.
        if (stream_to_abort) {
            stream_to_abort->abort();
        }
        if (br) {
            br->notify_closed();
            br->release();
        }
        for (auto *gate : to_release) {
            gate->count();
        }
    }

    struct OpItem {
        StreamReplyAdapter *adapter{nullptr};
        // Borrowed until the native operation retires; its gate resumes the owner.
        std::string_view data;
        bool is_finish{false};
        bool result{false};
        WFCounterTask *gate{nullptr};
    };

    class Awaiter : public coke::BasicAwaiter<bool> {
    public:
        Awaiter(StreamReplyAdapter *adapter, std::string_view data, bool is_finish)
            : adapter_(adapter) {
            item_ = new OpItem{adapter, data, is_finish, false, nullptr};
            WFCounterTask *task = WFTaskFactory::create_counter_task(1, [info = this->get_info(), item = this->item_](WFCounterTask *) {
                auto *awaiter = info->get_awaiter<Awaiter>();
                awaiter->emplace_result(item->result);
                delete item;
                awaiter->done();
            });
            item_->gate = task;
            this->set_task(task);
        }
        Awaiter(Awaiter &&that) noexcept
            : coke::BasicAwaiter<bool>(std::move(that)),
              adapter_(that.adapter_),
              item_(std::exchange(that.item_, nullptr)) {}

        template<typename PromiseType>
        void await_suspend(std::coroutine_handle<PromiseType> h) {
            auto *ad = adapter_;
            auto *it = std::exchange(item_, nullptr);
            coke::BasicAwaiter<bool>::await_suspend(h);
            ad->enqueue_op(it);
        }

        ~Awaiter() {
            if (item_) {
                delete item_;
            }
        }
    private:
        StreamReplyAdapter *adapter_;
        OpItem *item_{nullptr};
    };

    Awaiter async_write(std::string_view data) {
        return Awaiter(this, data, false);
    }

    Awaiter async_finish() {
        return Awaiter(this, std::string_view{}, true);
    }

private:
    void arm_close_callback() noexcept {
        if (!stream_ || !bridge_) return;
        bridge_->add_ref();
        int ret = stream_->set_close_callback(&StreamReplyAdapter::on_native_close, bridge_);
        if (ret == 0) {
            close_callback_armed_ = true;
        } else {
            close_callback_armed_ = false;
            bridge_->release();
            bridge_->notify_closed();
        }
    }

    bool disarm_close_callback_locked() noexcept {
        if (!close_callback_armed_ || !stream_) return false;
        close_callback_armed_ = false;
        bool cleared = stream_->clear_close_callback();
        if (cleared && bridge_) {
            bridge_->release();
        }
        return cleared;
    }

    static void on_native_close(void *context, int /* error */) noexcept {
        auto *bridge = static_cast<CloseBridge *>(context);
        bridge->notify_closed();
        bridge->release();
    }

    void enqueue_op(OpItem *item) {
        OpItem *to_submit = nullptr;
        ::StreamReply *stream_to_submit = nullptr;
        bool immediate_fail = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (aborted_ || finished_ || !stream_) {
                item->result = false;
                immediate_fail = true;
            } else if (in_flight_) {
                queue_.push_back(item);
                return;
            } else {
                in_flight_ = true;
                current_item_ = item;
                to_submit = item;
                stream_to_submit = stream_;
                if (item->is_finish) {
                    disarm_close_callback_locked();
                }
            }
        }
        if (immediate_fail) {
            if (item->gate) {
                item->gate->count();
            }
            return;
        }
        if (to_submit) {
            submit_op(stream_to_submit, to_submit);
        }
    }

    void submit_op(::StreamReply *stream, OpItem *item) {
        if (!stream) {
            on_callback(item, ECANCELED);
            return;
        }
        int ret = -1;
        if (item->is_finish) {
            ret = stream->finish(&StreamReplyAdapter::callback_thunk, item);
        } else {
            struct iovec iov = {
                .iov_base = const_cast<char *>(item->data.data()),
                .iov_len = item->data.size()
            };
            ret = stream->write(&iov, 1, &StreamReplyAdapter::callback_thunk, item);
        }
        if (ret < 0) {
            on_callback(item, errno ? errno : EPIPE);
        }
    }

    static void callback_thunk(void *context, int error) {
        auto *item = static_cast<OpItem *>(context);
        item->adapter->on_callback(item, error);
    }

    void on_callback(OpItem *item, int error) {
        OpItem *next_to_submit = nullptr;
        ::StreamReply *stream_to_submit = nullptr;
        std::vector<WFCounterTask *> to_release;
        WFCounterTask *item_gate = nullptr;
        CloseBridge *br = nullptr;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            item->result = (error == 0);
            item_gate = item->gate;
            current_item_ = nullptr;
            br = bridge_;
            if (br) br->add_ref();

            if (item->is_finish && error == 0) {
                finished_ = true;
                stream_ = nullptr;
            }

            if (error != 0) {
                aborted_ = true;
                disarm_close_callback_locked();
                stream_ = nullptr;
            }
            if (error != 0 || finished_) {
                // A successful finish is terminal too: queued writes cannot be submitted.
                while (!queue_.empty()) {
                    auto *queued = queue_.front();
                    queue_.pop_front();
                    queued->result = false;
                    if (queued->gate) {
                        to_release.push_back(queued->gate);
                    }
                }
                in_flight_ = false;
            } else if (!queue_.empty()) {
                next_to_submit = queue_.front();
                queue_.pop_front();
                current_item_ = next_to_submit;
                stream_to_submit = stream_;
                if (next_to_submit->is_finish) {
                    disarm_close_callback_locked();
                }
            } else {
                in_flight_ = false;
            }
        }

        if (error != 0 && br) {
            br->notify_closed();
        }
        if (br) {
            br->release();
        }

        if (next_to_submit) {
            submit_op(stream_to_submit, next_to_submit);
        }
        for (auto *gate : to_release) {
            gate->count();
        }
        if (item_gate) {
            item_gate->count();
        }
    }

private:
    ::StreamReply *stream_{nullptr};
    std::mutex mutex_;
    bool in_flight_{false};
    bool aborted_{false};
    bool finished_{false};
    bool close_callback_armed_{false};
    CloseBridge *bridge_{nullptr};
    OpItem *current_item_{nullptr};
    std::deque<OpItem *> queue_;
};

} // namespace kale

#endif // KALE_CORE_STREAM_REPLY_ADAPTER_H
