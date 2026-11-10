#include "example_server.h"

#include <cctype>
#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "kale/core/blueprint.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/core/router.h"

namespace {

std::optional<int> parse_int(std::string_view sv) {
    if (sv.empty()) return std::nullopt;
    int val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
        return val;
    }
    return std::nullopt;
}

std::string slugify(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool prev_dash = false;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            prev_dash = false;
        } else if (!prev_dash && !out.empty()) {
            out.push_back('-');
            prev_dash = true;
        }
    }
    if (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "untitled" : out;
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        examples::ServerOptions options(argc, argv);
        kale::HttpServer server;

        // Catalog blueprint: Query strings, colon path parameters (:id), and stateless operations
        kale::Blueprint catalog_bp;

        // Query parameters: ?category=hardware&page=1&limit=10
        catalog_bp.GET("/items", [](kale::Context &ctx) {
            std::string category = ctx.default_query("category", "all");
            std::string page = ctx.default_query("page", "1");
            std::string limit = ctx.default_query("limit", "10");

            std::string out = "Catalog Listing\n";
            out += "Filter Category: " + category + "\n";
            out += "Page: " + page + ", Limit: " + limit + "\n";
            out += "Items:\n";
            out += "  [101] Mechanical Keyboard (hardware)\n";
            out += "  [102] USB-C Dock (accessories)\n";
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string(out);
        }).name("catalog.items.list").tag("module", "catalog").tag("access", "public");

        // Colon named parameter (:id)
        catalog_bp.GET("/items/:id", [](kale::Context &ctx) {
            auto id = parse_int(ctx.param("id"));
            if (!id || *id <= 0) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string("Error: Item ID must be a positive integer\n");
                return;
            }

            if (*id > 500) {
                ctx.set_status(kale::HttpStatus::NotFound);
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string("Item " + std::to_string(*id) + " not found\n");
                return;
            }

            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Item ID: " + std::to_string(*id) + "\nTitle: Sample Catalog Item\nIn Stock: yes\n");
        }).name("catalog.items.get").tag("module", "catalog").tag("access", "public");

        // POST method demonstrating stateless pricing calculation from body input
        catalog_bp.POST("/quote", [](kale::Context &ctx) {
            std::string_view body = ctx.req_body();
            auto quantity = parse_int(body);
            if (!quantity || *quantity <= 0) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string("Error: Body must specify positive integer quantity\n");
                return;
            }

            double unit_price = 49.99;
            double total = *quantity * unit_price;
            ctx.set_status(kale::HttpStatus::OK);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Quote Calculation:\nQuantity: " + std::to_string(*quantity) +
                       "\nUnit Price: $" + std::to_string(unit_price) +
                       "\nTotal Estimated Cost: $" + std::to_string(total) + "\n");
        }).name("catalog.quote").tag("module", "catalog").tag("access", "public");

        // PUT method demonstrating stateless text transformation
        catalog_bp.PUT("/slugify", [](kale::Context &ctx) {
            std::string_view body = ctx.req_body();
            if (body.empty()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.set_content_type("text/plain; charset=utf-8");
                ctx.string("Error: Body text required for slug generation\n");
                return;
            }

            std::string slug = slugify(body);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Input: " + std::string(body) + "\nGenerated Slug: " + slug + "\n");
        }).name("catalog.slugify").tag("module", "catalog").tag("access", "public");

        // Articles blueprint: Brace parameters ({year}/{slug}) and wildcard (*filepath)
        kale::Blueprint articles_bp;

        articles_bp.GET("/posts/{year}/{slug}", [](kale::Context &ctx) {
            std::string year(ctx.param("year"));
            std::string slug(ctx.param("slug"));

            std::string out = "Article Lookup\n";
            out += "Year: " + year + "\n";
            out += "Slug: " + slug + "\n";
            out += "Headline: Modern C++ Routing with Kale\n";
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string(out);
        }).name("articles.posts.get").tag("module", "articles").tag("access", "public");

        articles_bp.GET("/files/*filepath", [](kale::Context &ctx) {
            std::string filepath(ctx.param("filepath"));
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("Requested Archive Path: " + filepath + "\nStatus: Available\n");
        }).name("articles.files.get").tag("module", "articles").tag("access", "public");

        // Nested Blueprint composition
        kale::Blueprint api_bp;
        api_bp.mount("/catalog", catalog_bp);
        api_bp.mount("/articles", articles_bp);
        server.mount("/api", api_bp);

        // Dynamic route enumeration returning adaptive aligned plain text table
        server.GET("/routes", [&server](kale::Context &ctx) {
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string(server.format_routes());
        }).name("server.routes.enumerate").tag("module", "system");

        // Custom 404 handler
        server.set_not_found_handler([](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::NotFound);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("404 Not Found: Path '" + std::string(ctx.path()) + "' does not match any route.\n");
        });

        // Custom 405 handler echoing RFC Allow header
        server.set_method_not_allowed_handler([](kale::Context &ctx) {
            ctx.set_status(kale::HttpStatus::MethodNotAllowed);
            ctx.set_content_type("text/plain; charset=utf-8");
            ctx.string("405 Method Not Allowed: " + std::string(ctx.method_string()) +
                       " is not supported for '" + std::string(ctx.path()) + "'.\nAllowed: " +
                       std::string(ctx.resp_header("Allow")) + "\n");
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
