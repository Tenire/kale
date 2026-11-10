#include <iostream>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <cstdlib>
#include <future>

#include <kale/core/http_server.h>
#include <kale/core/context.h>
#include <kale/http/sse.h>
#include <coke/queue.h>

struct EventHub {
    std::mutex mtx;
    std::vector<std::shared_ptr<coke::Queue<std::string>>> subscribers;

    // Snapshot active subscribers under lock, then push outside lock without holding mtx
    bool publish(const std::string &msg) {
        std::vector<std::shared_ptr<coke::Queue<std::string>>> active;
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (auto it = subscribers.begin(); it != subscribers.end(); ) {
                if ((*it)->closed()) {
                    it = subscribers.erase(it);
                } else {
                    active.push_back(*it);
                    ++it;
                }
            }
        }
        if (active.empty()) {
            return false;
        }
        bool all_pushed = true;
        for (auto &sub : active) {
            if (!sub->try_push(msg)) {
                all_pushed = false;
            }
        }
        return all_pushed;
    }

    void subscribe(std::shared_ptr<coke::Queue<std::string>> q) {
        std::lock_guard<std::mutex> lk(mtx);
        subscribers.push_back(std::move(q));
    }
};

int main(int argc, char **argv) {
    int port = 0;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    auto hub = std::make_shared<EventHub>();
    auto stop_promise = std::make_shared<std::promise<void>>();
    auto stop_fut = stop_promise->get_future();
    auto stop_flag = std::make_shared<std::once_flag>();

    kale::HttpServer server;

    server.GET("/health", [](kale::Context &ctx) {
        ctx.string("ok");
    });

    server.POST("/trigger", [hub](kale::Context &ctx) {
        std::string body(ctx.req_body());
        if (body.empty()) {
            body = "default_event";
        }
        bool ok = hub->publish(body);
        ctx.string(ok ? "event_queued" : "queue_full_or_no_subscribers");
    });

    server.GET("/events", [hub](kale::Context &ctx) -> coke::Task<void> {
        kale::SseEmitter sse(ctx);
        auto queue = std::make_shared<coke::Queue<std::string>>(64);

        // Subscribe before starting stream so registration is deterministic before client sees 200 OK
        hub->subscribe(queue);

        // consume arms headers immediately over the wire and pumps events until closed or disconnect
        co_await sse.consume(queue);
    });

    // Commit HTTP response over the wire via ctx.reply() before signaling main thread,
    // avoiding series-blocking shutdown deadlocks.
    server.POST("/stop", [stop_promise, stop_flag](kale::Context &ctx) -> coke::Task<void> {
        ctx.string("stopping");
        co_await ctx.reply();
        std::call_once(*stop_flag, [stop_promise]() {
            stop_promise->set_value();
        });
    });

    if (server.start("127.0.0.1", port) != 0) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }

    std::cout << "PORT=" << server.listen_port() << std::endl;
    std::cout.flush();

    // Main thread awaits stop signal
    stop_fut.wait();

    // Trigger native server stop from outside request Series.
    // Communicator stream shutdown terminates active quiet SSE streams cleanly.
    server.stop();
    return 0;
}
