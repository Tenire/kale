#include "example_server.h"

#include <iostream>
#include <string>
#include <string_view>

#include "coke/task.h"
#include "kale/core/blueprint.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // 1. Root Blueprint: /api/v1
        kale::Blueprint api_v1;

        // 2. Public Blueprint: /public with logging middleware
        kale::Blueprint public_bp;
        public_bp.use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            std::cout << "[nested_blueprints] [public_bp] Incoming " << ctx.method_string()
                      << " " << ctx.path() << std::endl;
            co_await next();
            std::cout << "[nested_blueprints] [public_bp] Response status: "
                      << ctx.status_code() << std::endl;
        });

        public_bp.GET("/info", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Public API v1 Information: Service operational.\n");
        });

        public_bp.GET("/status", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Public status: healthy\n");
        });

        // 3. Admin Blueprint: /admin with scoped AdminAuth middleware
        kale::Blueprint admin_bp;
        admin_bp.use([](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
            std::string token = ctx.header("X-Admin-Token");
            if (token != "secret-admin-pass") {
                ctx.set_status(kale::HttpStatus::Unauthorized);
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string("401 Unauthorized: Valid X-Admin-Token required.\n");
                co_return;
            }
            co_await next();
        });

        admin_bp.GET("/metrics", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Admin metrics: active_connections=12, memory_usage=64MB\n");
        });

        admin_bp.POST("/restart", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Admin action: graceful restart scheduled.\n");
        });

        // 4. User Blueprint: /users/:uid demonstrating parameter inheritance and sub-routes
        kale::Blueprint user_bp;
        user_bp.GET("/profile", [](kale::Context &ctx) {
            std::string uid = std::string(ctx.param("uid"));
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("User profile for UID: " + uid + "\n");
        });

        user_bp.GET("/settings", [](kale::Context &ctx) {
            std::string uid = std::string(ctx.param("uid"));
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("User settings for UID: " + uid + " (notifications=enabled, theme=dark)\n");
        });

        // Assemble hierarchy:
        // api_v1 mounts:
        //   - /public -> public_bp (/api/v1/public/info, /api/v1/public/status)
        //   - /admin  -> admin_bp  (/api/v1/admin/metrics, /api/v1/admin/restart)
        //   - /users/:uid -> user_bp (/api/v1/users/:uid/profile, /api/v1/users/:uid/settings)
        api_v1.mount("/public", std::move(public_bp));
        api_v1.mount("/admin", std::move(admin_bp));
        api_v1.mount("/users/:uid", std::move(user_bp));

        // Mount root blueprint onto server at /api/v1
        server.mount("/api/v1", std::move(api_v1));

        // Introspection endpoint returning formatted routing table
        // Demonstrates route metadata: accumulated middleware_count and max_body_size after flattening
        server.GET("/routes", [&server](kale::Context &ctx) {
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string(server.format_routes());
        });

        // Print formatted route table to stdout before serving
        std::cout << "Nested blueprint route topology:\n" << server.format_routes() << "\n";

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
