#include "example_server.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "coke/task.h"
#include "kale/core/context.h"
#include "kale/core/http_error.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/core/router.h"

namespace {

struct RequestTrace {
    std::string request_id;
    std::chrono::steady_clock::time_point start_time;
};

struct AuthenticatedUser {
    std::string username;
    std::string role; // "admin", "member"
    double discount_rate{0.0};
};

std::atomic<uint64_t> g_request_counter{1000};

std::optional<double> parse_double(std::string_view sv) {
    if (sv.empty()) return std::nullopt;
    double val = 0.0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
        return val;
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // Global middleware: correlation ID, execution timing, and HttpError unwinding
        server.use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            auto start_time = std::chrono::steady_clock::now();

            std::string req_id = ctx.header("X-Request-Id");
            if (req_id.empty()) {
                req_id = "req-" + std::to_string(g_request_counter.fetch_add(1));
            }

            ctx.set_attachment<RequestTrace>(RequestTrace{
                .request_id = req_id,
                .start_time = start_time
            });

            try {
                co_await next();
            } catch (const kale::HttpError &e) {
                ctx.set_status(e.status_code());
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string("HTTP Error " + std::to_string(e.status_code()) + ": " + e.what() + "\n");
            }

            const auto *trace = ctx.get_attachment<RequestTrace>();
            if (trace) {
                auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - trace->start_time
                ).count();

                ctx.set_header("X-Request-Id", trace->request_id);
                ctx.set_header("X-Response-Time-Us", std::to_string(elapsed_us));
            }
        });

        // Route-local middleware: Bearer token authentication and user context injection
        auto require_auth = [](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            std::string_view auth_hdr = ctx.header_view("Authorization");
            if (auth_hdr.empty() || !auth_hdr.starts_with("Bearer ")) {
                ctx.set_header("WWW-Authenticate", "Bearer realm=\"api\"");
                throw kale::HttpError(
                    kale::HttpStatus::Unauthorized,
                    "Missing or malformed Authorization header; expected 'Bearer <token>'"
                );
            }

            std::string_view token = auth_hdr.substr(7);
            if (token == "admin-token-secret") {
                ctx.set_attachment<AuthenticatedUser>(AuthenticatedUser{
                    .username = "alice_admin",
                    .role = "admin",
                    .discount_rate = 0.20 // 20% admin discount
                });
            } else if (token == "member-token-secret") {
                ctx.set_attachment<AuthenticatedUser>(AuthenticatedUser{
                    .username = "bob_member",
                    .role = "member",
                    .discount_rate = 0.05 // 5% member discount
                });
            } else {
                throw kale::HttpError(
                    kale::HttpStatus::Forbidden,
                    "Invalid or expired bearer token"
                );
            }

            co_await next();
        };

        // Public route: demonstrates global-only middleware execution
        server.GET("/public/status", [](kale::Context &ctx) {
            const auto *trace = ctx.get_attachment<RequestTrace>();
            std::string trace_id = trace ? trace->request_id : "none";
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Service Status: Operational\nCorrelation Trace ID: " + trace_id + "\n");
        });

        // Protected route: authorized stateless quote calculation
        server.POST("/quote/discounted",
            require_auth,
            [](kale::Context &ctx) {
                const auto *user = ctx.get_attachment<AuthenticatedUser>();
                if (!user) {
                    throw kale::HttpError(
                        kale::HttpStatus::InternalServerError,
                        "Authenticated user attachment missing from context"
                    );
                }

                std::string_view body = ctx.req_body();
                auto base_price = parse_double(body);
                if (!base_price || *base_price <= 0.0) {
                    throw kale::HttpError(
                        kale::HttpStatus::BadRequest,
                        "Request body must contain a positive numeric base price"
                    );
                }

                double discount_amount = *base_price * user->discount_rate;
                double final_price = *base_price - discount_amount;

                std::ostringstream oss;
                oss << std::fixed << std::setprecision(2);
                oss << "Authorized Discount Quote\n";
                oss << "User: " << user->username << " (Role: " << user->role << ")\n";
                oss << "Base Price: $" << *base_price << "\n";
                oss << "Discount Rate: " << static_cast<int>(user->discount_rate * 100) << "%\n";
                oss << "Discount Savings: -$" << discount_amount << "\n";
                oss << "Final Quoted Price: $" << final_price << "\n";

                ctx.set_status(kale::HttpStatus::OK);
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string(oss.str());
            }
        ).name("quote.discounted").tag("security", "bearer_token");

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
