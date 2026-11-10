#include "example_server.h"

#include <chrono>
#include <iostream>
#include <string>

#include "coke/sleep.h"
#include "coke/task.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // Minimal synchronous hello route: void(Context &ctx)
        server.GET("/", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Hello, Kale!\n");
        });

        // Coroutine-native route: coke::Task<void>(Context &ctx)
        // Demonstrates non-blocking coroutine suspension without thread blocking
        server.GET("/async", [](kale::Context &ctx) -> coke::Task<void> {
            co_await coke::sleep(std::chrono::milliseconds(10));

            std::string name = ctx.default_query("name", "World");
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Hello, " + name + " (from coroutine)!\n");
            co_return;
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
