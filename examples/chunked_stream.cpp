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

        // GET /export/csv: coroutine handler streaming 100 rows of CSV data in chunks
        // with simulated pauses (coke::sleep), checking send_chunk return status for disconnect.
        server.GET("/export/csv", [](kale::Context &ctx) -> coke::Task<void> {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/csv; charset=utf-8");
            ctx.set_header("Content-Disposition", "attachment; filename=\"report.csv\"");

            // Send CSV header row
            bool ok = co_await ctx.send_chunk("id,timestamp,metric_name,value,status\r\n");
            if (!ok) {
                std::cout << "[chunked_stream] Client disconnected before CSV header sent\n";
                co_return;
            }

            // Stream 100 rows in batches
            for (int i = 1; i <= 100; ++i) {
                // Check peer connection before emitting chunk
                if (!ctx.is_connected()) {
                    std::cout << "[chunked_stream] Client disconnected at row " << i << "\n";
                    co_return;
                }

                std::string row = std::to_string(i) + "," +
                                  "2026-11-08T12:00:00Z," +
                                  "cpu_load," +
                                  std::to_string(10.0 + (i % 50) * 1.5) + "," +
                                  (i % 10 == 0 ? "WARN" : "OK") + "\r\n";

                ok = co_await ctx.send_chunk(std::move(row));
                if (!ok) {
                    std::cout << "[chunked_stream] send_chunk returned false at row " << i << "\n";
                    co_return;
                }

                // Simulate processing/generation delay every 10 rows
                if (i % 10 == 0) {
                    co_await coke::sleep(std::chrono::milliseconds(20));
                }
            }

            // Signal stream completion (writes the zero-length terminal chunk)
            co_await ctx.end_chunk();
        });

        // GET /stream/ndjson: streaming newline-delimited JSON objects chunk-by-chunk
        server.GET("/stream/ndjson", [](kale::Context &ctx) -> coke::Task<void> {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("application/x-ndjson; charset=utf-8");

            const char *events[] = {
                "{\"seq\":1,\"event\":\"system_boot\",\"level\":\"info\"}\n",
                "{\"seq\":2,\"event\":\"db_connected\",\"level\":\"info\"}\n",
                "{\"seq\":3,\"event\":\"cache_warmup_start\",\"level\":\"debug\"}\n",
                "{\"seq\":4,\"event\":\"cache_warmup_complete\",\"level\":\"info\",\"keys_loaded\":1500}\n",
                "{\"seq\":5,\"event\":\"service_ready\",\"level\":\"info\",\"port\":8080}\n"
            };

            for (const char *evt : events) {
                if (!ctx.is_connected()) {
                    std::cout << "[chunked_stream] Client disconnected during NDJSON stream\n";
                    co_return;
                }

                bool ok = co_await ctx.send_chunk(evt);
                if (!ok) {
                    std::cout << "[chunked_stream] NDJSON send_chunk failed\n";
                    co_return;
                }

                // Brief delay between event notifications
                co_await coke::sleep(std::chrono::milliseconds(25));
            }

            co_await ctx.end_chunk();
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
