#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "coke/queue.h"
#include "coke/sleep.h"
#include "example_server.h"
#include "kale/core/http_server.h"
#include "kale/http/json.h"
#include "kale/http/sse.h"

namespace {

/**
 * @brief Thread-safe hub managing fan-out broadcasting to isolated subscriber queues.
 *
 * Each connected SSE subscriber owns an independent coke::Queue<SseEvent>.
 * When a message is published, it is pushed non-blockingly to all active subscriber
 * queues. If an individual subscriber's network is slow or queue is full, try_push
 * fails for that subscriber without blocking other subscribers or worker threads.
 */
class EventBroadcaster {
public:
    using QueueType = coke::Queue<kale::SseEvent>;
    using QueuePtr = std::shared_ptr<QueueType>;

    // Register a subscriber with dedicated queue (capacity: 64 events)
    QueuePtr add_subscriber() {
        auto q = std::make_shared<QueueType>(64);
        std::lock_guard<std::mutex> lock(mtx_);
        subscribers_.push_back(q);
        return q;
    }

    // Unregister and close subscriber queue on client disconnect
    void remove_subscriber(const QueuePtr &q) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
            if (*it == q) {
                subscribers_.erase(it);
                break;
            }
        }
        q->close();
    }

    // Broadcast an event to all subscribers without blocking
    size_t broadcast(std::string event_type, std::string payload) {
        std::lock_guard<std::mutex> lock(mtx_);
        uint64_t id = ++seq_counter_;

        kale::SseEvent ev{
            .id = std::to_string(id),
            .event = std::move(event_type),
            .data = std::move(payload),
            .retry = 3000,
            .comment = {},
        };
        size_t delivered = 0;
        for (const auto &sub : subscribers_) {
            if (sub->try_push(ev)) {
                ++delivered;
            }
        }
        return delivered;
    }

    size_t subscriber_count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return subscribers_.size();
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto &sub : subscribers_) {
            sub->close();
        }
        subscribers_.clear();
    }

private:
    mutable std::mutex mtx_;
    std::vector<QueuePtr> subscribers_;
    uint64_t seq_counter_{0};
};

// RAII guard ensuring subscriber queue is removed on handler exit or disconnect
struct SubscriberScopeGuard {
    std::shared_ptr<EventBroadcaster> hub;
    EventBroadcaster::QueuePtr queue;

    ~SubscriberScopeGuard() {
        if (hub && queue) {
            hub->remove_subscriber(queue);
        }
    }
};

} // namespace

int main(int argc, char *argv[]) {
    // 1. Process server options and install signal mask before starting coroutines
    examples::ServerOptions options(argc, argv);

    // 2. Configure HTTP server parameters
    kale::HttpServerParams params;
    params.set_keep_alive_timeout(60000)
          .set_request_size_limit(1024 * 1024); // 1MB limit for events/publish

    kale::HttpServer server(params);

    // Shared broadcaster instance managing isolated per-subscriber queues
    auto broadcaster = std::make_shared<EventBroadcaster>();

    // 3. SSE Subscriber Route: event-driven consumption from dedicated subscriber queue
    server.GET("/api/events", [broadcaster](kale::Context &ctx) -> coke::Task<void> {
        // Allocate isolated dedicated subscriber queue
        auto queue = broadcaster->add_subscriber();

        // Ensure registration is revoked when client disconnects or handler returns
        SubscriberScopeGuard guard{.hub = broadcaster, .queue = queue};

        kale::SseEmitter sse(ctx);

        // Send initial connection acknowledgement event
        kale::SseEvent welcome_ev{
            .id = "0",
            .event = "connected",
            .data = "{\"message\": \"Subscribed to Kale SSE event stream\"}",
            .retry = 3000,
            .comment = {},
        };
        if (!co_await sse.send_event(welcome_ev)) {
            co_return; // Client closed before welcome delivered
        }

        // Event-driven consumption: blocks on coroutine queue pop until new events arrive
        // SseEmitter::consume monitors connection state, closing queue and exiting cleanly upon disconnect
        co_await sse.consume(queue);
    });

    // 4. Publisher Route: pushes meaningful events to all active subscriber queues
    server.POST("/api/publish", [broadcaster](kale::Context &ctx) {
        std::string event_type = "message";
        std::string payload;

        if (ctx.is_json()) {
            try {
                auto j = kale::Json::parse(ctx.req_body());
                if (j.is_object()) {
                    if (j.has("event") && j["event"].is_string()) {
                        event_type = j["event"].get<std::string>();
                    }
                    if (j.has("data")) {
                        payload = j["data"].dump();
                    } else {
                        payload = j.dump();
                    }
                }
            } catch (...) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                kale::Json err;
                err["error"] = "Malformed JSON payload";
                ctx.json(err);
                return;
            }
        } else {
            payload = std::string(ctx.req_body());
        }

        if (payload.empty()) {
            payload = "{\"notice\": \"ping\"}";
        }

        size_t notified = broadcaster->broadcast(event_type, payload);

        kale::Json out;
        out["status"] = "published";
        out["event"] = event_type;
        out["subscribers_notified"] = static_cast<double>(notified);
        ctx.json(out);
    });

    // 5. Active subscriber inspection route
    server.GET("/api/subscribers", [broadcaster](kale::Context &ctx) {
        kale::Json out;
        out["active_subscribers"] = static_cast<double>(broadcaster->subscriber_count());
        ctx.json(out);
    });

    // 6. Native Chunked Streaming Route (progressive countdown stream)
    server.GET("/api/stream/countdown", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_content_type("text/plain; charset=utf-8");

        for (int i = 5; i >= 1; --i) {
            if (!ctx.is_connected()) {
                break; // Client closed connection early
            }

            std::string chunk = "Countdown: " + std::to_string(i) + "\n";
            bool ok = co_await ctx.send_chunk(chunk);
            if (!ok) {
                break;
            }

            // Asynchronously suspend coroutine without blocking worker threads
            co_await coke::sleep(std::chrono::milliseconds(250));
        }

        if (ctx.is_connected()) {
            co_await ctx.send_chunk("Liftoff!\n");
            co_await ctx.end_chunk();
        }
    });

    // 7. Informational index page
    server.GET("/", [](kale::Context &ctx) {
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string(
            "Kale Event Stream & Chunked Streaming Example\n"
            "============================================\n\n"
            "Endpoints:\n"
            "  GET  /api/events            - SSE event stream (blocks on dedicated coke::Queue)\n"
            "  POST /api/publish           - Publish event to all active SSE subscribers\n"
            "  GET  /api/subscribers       - Inspect active subscriber count\n"
            "  GET  /api/stream/countdown  - HTTP chunked transfer-encoding demonstration\n\n"
            "Interactive curl workflows:\n"
            "  # Terminal 1: Subscribe to SSE stream\n"
            "  curl -N http://127.0.0.1:8080/api/events\n\n"
            "  # Terminal 2: Publish an event\n"
            "  curl -i -X POST http://127.0.0.1:8080/api/publish \\\n"
            "       -H \"Content-Type: application/json\" \\\n"
            "       -d '{\"event\":\"alert\",\"data\":{\"service\":\"db\",\"status\":\"ok\"}}'\n\n"
            "  # Test chunked progressive streaming:\n"
            "  curl -N http://127.0.0.1:8080/api/stream/countdown\n"
        );
    });

    return examples::serve(server, options);
}
