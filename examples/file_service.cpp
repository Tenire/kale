#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "example_server.h"
#include "kale/core/http_server.h"
#include "kale/http/file_cache.h"
#include "kale/http/file_response.h"
#include "kale/http/static_files.h"

int main(int argc, char *argv[]) {
    // 1. Lifecycle initialization and signal mask installation
    examples::ServerOptions options(argc, argv);

    // 2. Resolve repository assets directory
    // Can be overridden via KALE_EXAMPLE_FILES environment variable
    std::string files_dir = "examples/assets";
    if (const char *env_dir = std::getenv("KALE_EXAMPLE_FILES")) {
        files_dir = env_dir;
    }

    if (!std::filesystem::is_directory(files_dir)) {
        std::cerr << "[file_service] Notice: Directory '" << files_dir
                  << "' not found relative to current working directory ("
                  << std::filesystem::current_path().string() << ").\n"
                  << "  Run from repository root or set KALE_EXAMPLE_FILES to the assets folder.\n";
    }

    // 3. Configure HTTP server parameters
    kale::HttpServerParams params;
    params.set_keep_alive_timeout(30000)
          .set_request_size_limit(10 * 1024 * 1024); // 10MB limit

    kale::HttpServer server(params);

    // 4. Mount direct uncached static file handler
    // Serves repository files with ETag, Last-Modified, and RFC 7233 Range support
    server.GET("/static/*filepath", kale::static_files(files_dir));
    server.HEAD("/static/*filepath", kale::static_files(files_dir));

    // 5. Mount in-memory LRU-cached static file handler
    // Caches file payloads up to 2MB in RAM; automatically invalidates on mtime changes
    kale::StaticFilesConfig cache_cfg;
    cache_cfg.root_dir = files_dir;
    cache_cfg.url_prefix = "/cached";
    cache_cfg.enable_cache = true;
    cache_cfg.cache_config.capacity = 512;
    cache_cfg.cache_config.max_file_size = 2 * 1024 * 1024;
    cache_cfg.cache_config.check_mtime = true;

    server.GET("/cached/*filepath", kale::static_files(cache_cfg));
    server.HEAD("/cached/*filepath", kale::static_files(cache_cfg));

    // 6. Asynchronous file download endpoint with Content-Disposition and RFC 7233 Range support
    server.GET("/download/:filename", [files_dir](kale::Context &ctx) -> coke::Task<void> {
        std::string_view filename = ctx.param("filename");

        // Basic traversal check: prevent directory separator escaping
        if (filename.empty() || filename.find('/') != std::string_view::npos ||
            filename.find('\\') != std::string_view::npos || filename == ".." || filename == ".") {
            ctx.set_status(kale::HttpStatus::Forbidden).string("403 Forbidden: Invalid filename\n");
            co_return;
        }

        std::filesystem::path target = std::filesystem::path(files_dir) / filename;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(target, ec)) {
            ctx.set_status(kale::HttpStatus::NotFound).string("404 Not Found: File not found\n");
            co_return;
        }

        ctx.set_header("Content-Disposition", "attachment; filename=\"" + std::string(filename) + "\"");

        // Asynchronously stream full file or partial byte ranges per RFC 7233
        if (ctx.has_header("Range")) {
            co_await kale::send_file_range(ctx, target.string(), ctx.header("Range"));
        } else {
            co_await kale::send_file(ctx, target.string());
        }
    });

    // 7. Informational root dashboard
    server.GET("/", [files_dir](kale::Context &ctx) {
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string(
            "Kale File Service Example\n"
            "=========================\n"
            "Assets directory: " + files_dir + "\n\n"
            "Routes:\n"
            "  GET  /static/*filepath     - Uncached static file serving\n"
            "  HEAD /static/*filepath     - Static file metadata probe\n"
            "  GET  /cached/*filepath     - In-memory LRU-cached static files\n"
            "  GET  /download/:filename   - Direct download with RFC 7233 Range support\n\n"
            "Sample curl commands:\n"
            "  curl -i http://127.0.0.1:8080/static/hello.txt\n"
            "  curl -i http://127.0.0.1:8080/cached/index.html\n"
            "  curl -i -H \"Range: bytes=0-25\" http://127.0.0.1:8080/download/hello.txt\n"
        );
    });

    // 8. Run server and block on signals
    return examples::serve(server, options);
}
