#include "example_server.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "kale/core/context.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/core/logger.h"
#include "kale/http/json.h"
#include "kale/middleware/error_handler.h"

namespace {

class ResourceNotFoundException : public std::runtime_error {
public:
    explicit ResourceNotFoundException(std::string resource)
        : std::runtime_error("Resource '" + resource + "' was not found"),
          resource_(std::move(resource)) {}

    [[nodiscard]] const std::string &resource() const noexcept { return resource_; }

private:
    std::string resource_;
};

class PaymentRequiredException : public std::runtime_error {
public:
    PaymentRequiredException(std::string reason, double amount_due)
        : std::runtime_error("Payment required: " + reason),
          reason_(std::move(reason)),
          amount_due_(amount_due) {}

    [[nodiscard]] const std::string &reason() const noexcept { return reason_; }
    [[nodiscard]] double amount_due() const noexcept { return amount_due_; }

private:
    std::string reason_;
    double amount_due_{0.0};
};

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);

        kale::HttpServerParams params;
        params.set_logger(kale::Logger(kale::LogLevel::Info));

        // Fallback policy for uncaught core/routing errors (e.g. 404/405/500 outside middleware)
        params.error_policy.expose_internal_errors = false;
        params.error_policy.default_blurred_message = "Internal Server Error";
        params.error_policy.logger = params.logger;
        params.set_error_formatter(kale::ErrorFormatters::problem_details());

        kale::HttpServer server(params);

        // Outermost: Centralized error_handler middleware catching unhandled exceptions
        // Demonstrates default standard_json formatting for downstream application exceptions
        server.use(kale::error_handler(kale::ErrorFormatters::standard_json(), params.error_policy));
        // Domain exception mapping middleware: translate business exceptions to HttpError / reply_error
        server.use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            try {
                co_await next();
            } catch (const ResourceNotFoundException &e) {
                auto err = kale::HttpError::not_found(e.what())
                               .with_detail("resource", e.resource());
                kale::reply_error(ctx, err, kale::ErrorFormatters::problem_details(), {});
            } catch (const PaymentRequiredException &e) {
                auto err = kale::HttpError(402, e.what(), "PAYMENT_REQUIRED")
                               .with_detail("reason", e.reason())
                               .with_detail("amount_due", std::to_string(e.amount_due()));
                kale::reply_error(ctx, err, kale::ErrorFormatters::problem_details(), {});
            }
        });
        // Service root
        server.GET("/", [](kale::Context &ctx) {
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Kale Error Handling Demonstration\n"
                       "Available endpoints:\n"
                       "  GET  /items/:id               - Domain exception mapped via reply_error\n"
                       "  POST /billing/charge          - Domain PaymentRequired mapped via reply_error\n"
                       "  GET  /direct/json             - Direct write via reply_error (standard_json)\n"
                       "  GET  /direct/text             - Direct write via reply_error (plain_text)\n"
                       "  GET  /direct/problem          - Direct write via reply_error (problem_details)\n"
                       "  GET  /auth/jwt-required       - HttpError::unauthorized with Bearer realm\n"
                       "  GET  /api/rate-limited        - HttpError::too_many_requests with Retry-After\n"
                       "  POST /validate                - HttpError::unprocessable_entity with field details\n"
                       "  GET  /rpc/error               - RPC mode (always_http_200 = true)\n"
                       "  GET  /crash                   - Unhandled crash blurred to client, logged to internal logger\n");
        });

        // Domain exception routes
        server.GET("/items/:id", [](kale::Context &ctx) {
            std::string id = std::string(ctx.param("id"));
            if (id != "42") {
                throw ResourceNotFoundException("item:" + id);
            }
            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(kale::Json::Object{{"id", 42}, {"name", "The Answer"}});
        });

        server.POST("/billing/charge", []() {
            throw PaymentRequiredException("Subscription quota exceeded", 19.99);
        });

        // Direct reply_error routes with explicit formatters
        server.GET("/direct/json", [](kale::Context &ctx) {
            auto err = kale::HttpError::bad_request("Malformed query string", "BAD_QUERY");
            kale::reply_error(ctx, err, kale::ErrorFormatters::standard_json(), {});
        });

        server.GET("/direct/text", [](kale::Context &ctx) {
            auto err = kale::HttpError::conflict("Resource lock held by another process", "LOCK_CONFLICT");
            kale::reply_error(ctx, err, kale::ErrorFormatters::plain_text(), {});
        });

        server.GET("/direct/problem", [](kale::Context &ctx) {
            auto err = kale::HttpError::forbidden("Access forbidden to requested scope", "INSUFFICIENT_SCOPE");
            kale::reply_error(ctx, err, kale::ErrorFormatters::problem_details(), {});
        });

        // RFC protocol headers on HttpError
        server.GET("/auth/jwt-required", [](kale::Context &ctx) {
            auto err = kale::HttpError::unauthorized("Bearer token is missing or expired", "TOKEN_EXPIRED", "kale-secure-api");
            kale::reply_error(ctx, err);
        });

        server.GET("/api/rate-limited", [](kale::Context &ctx) {
            auto err = kale::HttpError::too_many_requests("Rate limit exceeded for client", 60);
            kale::reply_error(ctx, err);
        });

        server.POST("/validate", [](kale::Context &ctx) {
            auto err = kale::HttpError::unprocessable_entity("Payload failed validation rules")
                           .with_detail("username", "must be at least 3 characters")
                           .with_detail("email", "invalid domain format");
            kale::reply_error(ctx, err);
        });

        // RPC-over-HTTP mode: returns wire HTTP 200 OK while preserving error status in envelope
        server.GET("/rpc/error", [](kale::Context &ctx) {
            kale::ErrorPolicy rpc_policy;
            rpc_policy.always_http_200 = true;
            auto err = kale::HttpError::bad_request("Invalid RPC method parameters", "RPC_INVALID_PARAMS");
            kale::reply_error(ctx, err, kale::ErrorFormatters::standard_json(), rpc_policy);
        });

        // Uncaught exception escaping to error_handler middleware
        server.GET("/crash", []() {
            throw std::runtime_error("Sensitive database connection credentials failed on host 10.0.4.12:5432");
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
