#include "example_server.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

#include "kale/core/context.h"
#include "kale/core/http_error.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/core/logger.h"
#include "kale/http/json.h"
#include "kale/middleware/access_log.h"
#include "kale/middleware/request_id.h"

namespace {

// Synchronized access log sink formatting each HTTP transaction record
class ConsoleAccessSink {
public:
    void operator()(const kale::AccessLogRecord &rec) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "[ACCESS_LOG] "
                  << rec.method << " " << rec.target
                  << " -> " << rec.status_code
                  << " (" << rec.duration.count() << " us)"
                  << (rec.request_id.empty() ? "" : " id=" + rec.request_id)
                  << (rec.downstream_exception ? " [DOWNSTREAM_EXCEPTION]" : "")
                  << "\n";
    }

private:
    std::mutex mutex_;
};

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);

        // Declare log sink BEFORE server so it outlives server and any in-flight teardown tasks
        ConsoleAccessSink access_sink;

        // Configure internal framework Logger for diagnostic and uncaught error events
        kale::Logger internal_logger(kale::LogLevel::Info);

        kale::HttpServerParams params;
        params.set_logger(internal_logger);

        kale::HttpServer server(params);

        // Register request_id middleware first so correlation ID is available to access_log
        server.use(kale::request_id());

        // Register access_log middleware to record every HTTP transaction
        server.use(kale::access_log([&access_sink](kale::AccessLogRecord rec) {
            access_sink(rec);
        }));
        // Normal endpoints
        server.GET("/", [](kale::Context &ctx) {
            ctx.json(kale::Json::Object{
                {"service", "access_log_example"},
                {"status", "healthy"},
                {"request_id", std::string(kale::get_request_id(ctx))}
            });
        });

        server.GET("/items/:id", [](kale::Context &ctx) {
            std::string_view id = ctx.param("id");
            if (id == "404") {
                throw kale::HttpError::not_found("Item not found", "ITEM_NOT_FOUND");
            }
            ctx.json(kale::Json::Object{
                {"item_id", std::string(id)},
                {"name", "Sample Item"},
                {"request_id", std::string(kale::get_request_id(ctx))}
            });
        });

        // Failing endpoint: triggers uncaught std::runtime_error
        // Escapes to HttpServer::process, which logs the exception via internal Logger
        // and returns 500 Internal Server Error, while access_log records the transaction.
        server.GET("/error/crash", [](kale::Context &/*ctx*/) {
            throw std::runtime_error("Simulated unhandled database connection failure");
        });

        // Failing endpoint: returns 400 Bad Request via HttpError factory
        server.GET("/error/bad-request", [](kale::Context &/*ctx*/) {
            throw kale::HttpError::bad_request("Invalid query parameters", "INVALID_QUERY");
        });

        // Failing endpoint: triggers 429 Too Many Requests with Retry-After header
        server.GET("/error/rate-limited", [](kale::Context &/*ctx*/) {
            throw kale::HttpError::too_many_requests("Quota exceeded; slow down", 30, "QUOTA_EXCEEDED");
        });

        std::cout << "Starting example_access_log with access logging and internal diagnostic logger...\n";
        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
