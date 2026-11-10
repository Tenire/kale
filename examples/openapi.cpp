#include "example_server.h"

#include <iostream>
#include <string>

#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/http/binding.h"
#include "kale/http/field_desc.h"
#include "kale/http/json.h"
#include "kale/http/openapi.h"
#include "kale/http/validator.h"

namespace {

using namespace kale::rules;

struct UserCreateDTO {
    std::string username;
    std::string email;
    std::string role{"user"};
    int age{18};

    KALE_BIND(
        KALE_FIELD(username, min_len(3), max_len(30)),
        KALE_FIELD(email, is_email()),
        KALE_FIELD(role, one_of<std::string>({"user", "admin", "moderator"})),
        KALE_FIELD(age, gte(18), lte(120))
    )
};

struct UserResponseDTO {
    int id{0};
    std::string username;
    std::string email;
    std::string role;
    int age{0};

    KALE_BIND(
        KALE_FIELD(id, gt(0)),
        KALE_FIELD(username),
        KALE_FIELD(email),
        KALE_FIELD(role),
        KALE_FIELD(age)
    )
};

const char *kSwaggerUiHtml = R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Kale OpenAPI 3.0 Documentation</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css" />
  <style>
    body { margin: 0; background: #fafafa; }
  </style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
  <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-standalone-preset.js"></script>
  <script>
    window.onload = function() {
      SwaggerUIBundle({
        url: "/openapi.json",
        dom_id: '#swagger-ui',
        deepLinking: true,
        presets: [
          SwaggerUIBundle.presets.apis,
          SwaggerUIStandalonePreset
        ],
        layout: "StandaloneLayout"
      });
    };
  </script>
</body>
</html>)html";

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // Register application routes
        server.GET("/api/users", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(kale::Json::Array{
                kale::Json::Object{
                    {"id", 1},
                    {"username", "alice"},
                    {"email", "alice@example.com"},
                    {"role", "admin"},
                    {"age", 30}
                },
                kale::Json::Object{
                    {"id", 2},
                    {"username", "bob"},
                    {"email", "bob@example.com"},
                    {"role", "user"},
                    {"age", 25}
                }
            });
        });

        server.GET("/api/users/:id", [](kale::Context &ctx) {
            int id = 0;
            try {
                id = std::stoi(std::string(ctx.param("id")));
            } catch (...) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "Invalid user ID"}
                });
                return;
            }

            if (id == 1) {
                ctx.set_status(kale::HttpStatus::OK);
                ctx.json(kale::Json::Object{
                    {"id", 1},
                    {"username", "alice"},
                    {"email", "alice@example.com"},
                    {"role", "admin"},
                    {"age", 30}
                });
                return;
            }

            if (id == 2) {
                ctx.set_status(kale::HttpStatus::OK);
                ctx.json(kale::Json::Object{
                    {"id", 2},
                    {"username", "bob"},
                    {"email", "bob@example.com"},
                    {"role", "user"},
                    {"age", 25}
                });
                return;
            }

            ctx.set_status(kale::HttpStatus::NotFound);
            ctx.json(kale::Json::Object{
                {"error", "User not found"},
                {"id", id}
            });
        });

        server.POST("/api/users", [](kale::Context &ctx) {
            UserCreateDTO dto;
            try {
                dto = ctx.bind_json<UserCreateDTO>();
            } catch (const kale::JsonBindError &err) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "Validation failure"},
                    {"field", err.field()},
                    {"message", err.what()}
                });
                return;
            }

            ctx.set_status(kale::HttpStatus::Created);
            ctx.json(kale::Json::Object{
                {"id", 101},
                {"username", dto.username},
                {"email", dto.email},
                {"role", dto.role},
                {"age", dto.age}
            });
        });

        // Associate routes with OpenApiRegistry
        kale::OpenApiRegistry registry;

        registry.associate(kale::HttpMethod::GET, "/api/users")
            .set_summary("List users")
            .set_description("Retrieves all registered users")
            .add_tag("Users")
            .response<UserResponseDTO>(200, "List of users");

        registry.associate(kale::HttpMethod::GET, "/api/users/:id")
            .set_summary("Get user by ID")
            .set_description("Returns details for a single user by primary key")
            .add_tag("Users")
            .response<UserResponseDTO>(200, "User found")
            .response(400, "Invalid ID format")
            .response(404, "User not found");

        registry.associate(kale::HttpMethod::POST, "/api/users")
            .set_summary("Create user")
            .set_description("Validates UserCreateDTO and creates a new user account")
            .add_tag("Users")
            .body<UserCreateDTO>("New user information")
            .response<UserResponseDTO>(200, "User successfully created")
            .response(400, "Validation failure");

        // Generate OpenAPI 3.0 specification
        kale::OpenApiConfig config{
            .title = "User Management Service API",
            .version = "1.0.0",
            .description = "Demonstration of OpenAPI 3.0 generation and Swagger UI in Kale"
        };

        kale::OpenApiDocument openapi_doc = kale::generate_openapi(
            config,
            server.router().routes(),
            registry);

        server.GET("/", [](kale::Context& ctx) -> void {
            ctx.redirect("/docs");
        });

        // Mount GET /openapi.json using kale::make_openapi_handler
        server.GET("/openapi.json", kale::make_openapi_handler(std::move(openapi_doc)));

        // Mount GET /docs (Swagger UI)
        server.GET("/docs", [](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/html; charset=utf-8");
            ctx.string(kSwaggerUiHtml);
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
