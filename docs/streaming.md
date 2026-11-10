# Server-Sent Events (SSE) & Chunked Streaming

This guide explains how to implement event-driven Server-Sent Events (SSE) and HTTP chunked transfer-encoding streams in Kale. It demonstrates subscriber isolation using dedicated coroutine queues (`coke::Queue`), clean disconnect teardown, and non-blocking pub/sub broadcasting without artificial polling loops.

The complete runnable code for this guide is located at [`examples/event_stream.cpp`](../examples/event_stream.cpp).

---

## Building and Running

Compile and launch the event stream example using native `xmake`:

```bash
# Build the binary target
xmake build example_event_stream

# Run on default port 8080 from repository root
xmake run example_event_stream

# Or run with an OS-assigned ephemeral port
xmake run example_event_stream 0
```

When started, the service prints:
```
Listening on http://127.0.0.1:8080/
```

Press `Ctrl+C` or send `SIGTERM` to gracefully stop the service.

---

## Architecture & Minimal Libraries

| Component | Header | Library | Description |
|---|---|---|---|
| `SseEmitter` / `SseEvent` | `#include "kale/http/sse.h"` | `kale_http` | Formats SSE frames and drives coroutine queue consumption |
| `coke::Queue` | `#include "coke/queue.h"` | `coke` (via `kale_core`) | Coroutine-native FIFO queue with non-blocking push and awaitable pop |
| `coke::sleep` | `#include "coke/sleep.h"` | `coke` (via `kale_core`) | Asynchronous non-blocking coroutine sleep awaiter |
| `Context` | `#include "kale/core/context.h"` | `kale_core` | Core request/response context with `send_chunk` and disconnect detection |
| `Json` | `#include "kale/http/json.h"` | `kale_http` | JSON parsing and serialization |

---

## Event-Driven SSE vs. Polling

Many naive SSE implementations use infinite loops with `sleep(1)` to poll shared data structures. This wastes CPU cycles, increases event latency, and fails to scale.

Kale uses an **event-driven, queue-backed model**:

```
[Publisher] ---> POST /api/publish
                     |
                     +--> EventBroadcaster::broadcast()
                              |
                              +---> Subscriber 1 Queue (coke::Queue) ---> SseEmitter::consume() ---> Client 1
                              |
                              +---> Subscriber 2 Queue (coke::Queue) ---> SseEmitter::consume() ---> Client 2
```

1. **Dedicated Per-Subscriber Queue**: When a client connects to `GET /api/events`, the server allocates an independent `coke::Queue<SseEvent>`.
2. **True Event-Driven Sleep**: The handler awaits `sse.consume(queue)`, which internally calls `co_await queue->pop()`. The coroutine is suspended with zero CPU usage until a publisher pushes an event into the queue.
3. **Subscriber Isolation**: Broadcasters push events non-blockingly using `queue->try_push(ev)`. If one subscriber is slow or its network buffer is saturated, only that subscriber's queue drops events or fails; all other subscribers and worker threads proceed without delay.

---

## Connection Teardown & Lifetime Management

Managing connection state and preventing resource leaks when clients disconnect is critical for long-lived streams:

```cpp
#include <memory>
#include <vector>

#include "coke/queue.h"
#include "coke/task.h"

// Convenience headers (active):
#include <kale/core.h>
#include <kale/stream.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"
// #include "kale/http/sse.h"

class SampleEventBroadcaster {
public:
    using QueueType = coke::Queue<kale::SseEvent>;
    using QueuePtr = std::shared_ptr<QueueType>;

    QueuePtr add_subscriber() {
        auto q = std::make_shared<QueueType>(64);
        subscribers_.push_back(q);
        return q;
    }

    void remove_subscriber(const QueuePtr &q) {
        for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
            if (*it == q) {
                subscribers_.erase(it);
                break;
            }
        }
        q->close();
    }

private:
    std::vector<QueuePtr> subscribers_;
};

struct SampleSubscriberGuard {
    std::shared_ptr<SampleEventBroadcaster> hub;
    SampleEventBroadcaster::QueuePtr queue;

    ~SampleSubscriberGuard() {
        if (hub && queue) {
            hub->remove_subscriber(queue);
        }
    }
};

void configure_sse_subscriber(kale::HttpServer &server, std::shared_ptr<SampleEventBroadcaster> broadcaster) {
    server.GET("/api/events", [broadcaster](kale::Context &ctx) -> coke::Task<void> {
        auto queue = broadcaster->add_subscriber();
        SampleSubscriberGuard guard{.hub = broadcaster, .queue = queue};

        kale::SseEmitter sse(ctx);
        kale::SseEvent welcome_ev{
            .id = "0",
            .event = "connected",
            .data = "{\"message\": \"Subscribed to Kale SSE event stream\"}",
            .retry = 3000,
            .comment = {},
        };
        if (!co_await sse.send_event(welcome_ev)) {
            co_return;
        }

        co_await sse.consume(queue);
    });
}
```

### Connection Teardown & Queue Termination

Inside `SseEmitter::consume`:
- Kale's SSE module monitors connection state and transport teardown.
- When the remote client disconnects (or the network drops), the consumer terminates immediately and closes the subscriber's `coke::Queue`.
- Any awaiting `queue->pop()` wakes up and returns `coke::TOP_CLOSED`, ending `sse.consume()`.
- The stack unwinds, `SubscriberScopeGuard` unregisters the queue from the broadcaster, and all subscriber resources are cleanly freed without dangling handles.

---

## Native HTTP Chunked Progressive Streaming

For streaming non-SSE binary or text data in chunked transfer-encoding slices (e.g. progressive generation, countdowns, large log tails), use `Context::send_chunk` and `Context::end_chunk`:

```cpp
#include <chrono>
#include <string>

#include "coke/sleep.h"
#include "coke/task.h"

// Convenience headers (active):
#include <kale/core.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/core/http_server.h"

void configure_chunked_stream(kale::HttpServer &server) {
    server.GET("/api/stream/countdown", [](kale::Context &ctx) -> coke::Task<void> {
        ctx.set_content_type("text/plain; charset=utf-8");

        for (int i = 5; i >= 1; --i) {
            if (!ctx.is_connected()) {
                break;
            }

            std::string chunk = "Countdown: " + std::to_string(i) + "\n";
            bool ok = co_await ctx.send_chunk(chunk);
            if (!ok) {
                break;
            }

            co_await coke::sleep(std::chrono::milliseconds(250));
        }

        if (ctx.is_connected()) {
            co_await ctx.send_chunk("Liftoff!\n");
            co_await ctx.end_chunk();
        }
    });
}
```

`Context::send_chunk` sets `Transfer-Encoding: chunked` automatically on the first chunk and flushes data to the socket immediately.

---

## Interactive `curl` Workflows

### 1. Subscribing to the SSE Event Stream

In a dedicated terminal, connect to `/api/events`:

```bash
# Note: -N / --no-buffer disables curl's internal buffering
curl -N http://127.0.0.1:8080/api/events
```

Output:
```http
event: connected
id: 0
retry: 3000
data: {"message": "Subscribed to Kale SSE event stream"}

```

The connection stays open, waiting for events.

### 2. Publishing an Event

In a second terminal, send a POST request with an event payload:

```bash
curl -i -X POST http://127.0.0.1:8080/api/publish \
     -H "Content-Type: application/json" \
     -d '{"event": "system_alert", "data": {"service": "database", "status": "reconnected"}}'
```

Publisher Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "published",
  "event": "system_alert",
  "subscribers_notified": 1
}
```

Subscriber Terminal Output (delivered in real time):
```http
event: system_alert
id: 1
retry: 3000
data: {"service": "database", "status": "reconnected"}

```

### 3. Inspecting Active Subscribers

```bash
curl -i http://127.0.0.1:8080/api/subscribers
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "active_subscribers": 1
}
```

When you close the subscriber curl terminal (`Ctrl+C`), query `/api/subscribers` again to verify clean unregistration:
```json
{
  "active_subscribers": 0
}
```

### 4. Progressive Chunked Streaming

```bash
curl -N http://127.0.0.1:8080/api/stream/countdown
```

Output (emitted smoothly over 1.25 seconds):
```
Countdown: 5
Countdown: 4
Countdown: 3
Countdown: 2
Countdown: 1
Liftoff!
```

---

## Protocol & Security Limits

1. **Finite Buffer Size**: `coke::Queue` instances are created with a bounded capacity (e.g. 64 events). If a consumer stalls, new events are dropped rather than exhausting process memory.
2. **Keep-Alive Headers**: `SseEmitter` sets `Content-Type: text/event-stream` and `Cache-Control: no-cache` automatically, preventing proxy buffering.
3. **Graceful Disconnect Detection**: Never rely solely on client cooperation to terminate streams; `ctx.is_connected()` and `sse.is_connected()` detect remote socket closures and abort immediately.
