#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "example_server.h"
#include "kale/core/http_server.h"
#include "kale/http/file_response.h"
#include "kale/http/json.h"
#include "kale/http/multipart.h"
#include "kale/http/upload.h"

int main(int argc, char *argv[]) {
    // 1. Process server options and set up signal mask before spawning worker threads
    examples::ServerOptions options(argc, argv);

    // 2. Resolve destination directory for uploaded files
    std::string upload_dir = "build/example-uploads";
    if (const char *env_dir = std::getenv("KALE_EXAMPLE_UPLOAD_DIR")) {
        upload_dir = env_dir;
    }

    std::error_code ec;
    std::filesystem::create_directories(upload_dir, ec);
    if (ec) {
        std::cerr << "[uploads] Error: Failed to create upload directory '" << upload_dir
                  << "': " << ec.message() << "\n";
        return 1;
    }

    // 3. Configure HTTP server parameters
    //
    // Note on Request Lifecycle & Buffering:
    // Standard HTTP multipart/form-data requests in Kale are bounded buffered in memory
    // by the HTTP protocol parser before handler coroutines execute (capped by
    // request_size_limit). The asynchronous persistence helper `save_form_file`
    // non-blockingly writes this buffered data to disk via coke::pwrite.
    // Request bodies in Kale are buffered per request; there is no single-request
    // ingress streaming. For uploading large datasets across multiple bounded
    // requests with direct-offset disk writes, see resumable_upload.cpp.
    kale::HttpServerParams params;
    params.set_keep_alive_timeout(30000)
          .set_request_size_limit(16 * 1024 * 1024); // 16 MB max body limit

    kale::HttpServer server(params);

    // 4. Multipart upload route: parses multipart/form-data and saves files asynchronously
    server.POST("/api/upload", [upload_dir](kale::Context &ctx) -> coke::Task<void> {
        auto files = kale::parse_multipart(ctx);
        if (files.empty()) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "No multipart files received. Ensure Content-Type is multipart/form-data with valid boundary.";
            ctx.json(err);
            co_return;
        }

        kale::Json result;
        result["status"] = "success";
        kale::Json uploaded_list = kale::Json::parse("[]");

        for (const auto &file : files) {
            std::string_view raw_name = file.filename.empty() ? std::string_view(file.name) : std::string_view(file.filename);

            // Strip any directory paths to prevent traversal
            size_t last_sep = raw_name.find_last_of("/\\");
            std::string_view base_name = (last_sep == std::string_view::npos) ? raw_name : raw_name.substr(last_sep + 1);

            std::string safe_name;
            if (base_name.empty() || base_name == "." || base_name == "..") {
                safe_name = "upload_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".bin";
            } else {
                safe_name = std::string(base_name);
            }

            std::filesystem::path dest_path = std::filesystem::path(upload_dir) / safe_name;

            // Non-blocking asynchronous file persistence via coke::pwrite
            int save_ret = co_await kale::save_form_file(file, dest_path.string());
            if (save_ret != 0) {
                ctx.set_status(kale::HttpStatus::InternalServerError);
                kale::Json fail_json;
                fail_json["error"] = "Failed to write file: " + safe_name;
                fail_json["code"] = save_ret;
                ctx.json(fail_json);
                co_return;
            }

            kale::Json item;
            item["field"] = file.name;
            item["filename"] = safe_name;
            item["content_type"] = file.content_type;
            item["size_bytes"] = static_cast<double>(file.data.size());
            item["saved_path"] = dest_path.string();
            uploaded_list.push_back(item);
        }

        result["files"] = uploaded_list;
        ctx.set_status(kale::HttpStatus::Created);
        ctx.json(result);
    });

    // 5. List uploaded files
    server.GET("/api/uploads", [upload_dir](kale::Context &ctx) {
        kale::Json items = kale::Json::parse("[]");
        std::error_code dir_ec;

        if (std::filesystem::exists(upload_dir, dir_ec)) {
            for (const auto &entry : std::filesystem::directory_iterator(upload_dir, dir_ec)) {
                if (entry.is_regular_file()) {
                    kale::Json f;
                    f["filename"] = entry.path().filename().string();
                    f["size_bytes"] = static_cast<double>(entry.file_size());
                    items.push_back(f);
                }
            }
        }

        kale::Json out;
        out["directory"] = upload_dir;
        out["files"] = items;
        ctx.json(out);
    });

    // 6. Download an uploaded file with RFC 7233 Range support
    server.GET("/api/uploads/:filename", [upload_dir](kale::Context &ctx) -> coke::Task<void> {
        std::string_view filename = ctx.param("filename");
        if (filename.empty() || filename.find('/') != std::string_view::npos ||
            filename.find('\\') != std::string_view::npos || filename == ".." || filename == ".") {
            ctx.set_status(kale::HttpStatus::Forbidden).string("403 Forbidden: Invalid file path\n");
            co_return;
        }

        std::filesystem::path target = std::filesystem::path(upload_dir) / filename;
        std::error_code chk_ec;
        if (!std::filesystem::is_regular_file(target, chk_ec)) {
            ctx.set_status(kale::HttpStatus::NotFound).string("404 Not Found: Uploaded file not found\n");
            co_return;
        }

        if (ctx.has_header("Range")) {
            co_await kale::send_file_range(ctx, target.string(), ctx.header("Range"));
        } else {
            co_await kale::send_file(ctx, target.string());
        }
    });

    // 7. Delete an uploaded file
    server.DELETE("/api/uploads/:filename", [upload_dir](kale::Context &ctx) {
        std::string_view filename = ctx.param("filename");
        if (filename.empty() || filename.find('/') != std::string_view::npos ||
            filename.find('\\') != std::string_view::npos || filename == ".." || filename == ".") {
            ctx.set_status(kale::HttpStatus::Forbidden).string("403 Forbidden: Invalid file path\n");
            return;
        }

        std::filesystem::path target = std::filesystem::path(upload_dir) / filename;
        std::error_code rm_ec;
        if (!std::filesystem::remove(target, rm_ec) || rm_ec) {
            ctx.set_status(kale::HttpStatus::NotFound).string("404 Not Found: Could not delete file\n");
            return;
        }

        kale::Json res;
        res["status"] = "deleted";
        res["filename"] = std::string(filename);
        ctx.json(res);
    });

    // 8. Service index and curl instructions
    server.GET("/", [upload_dir](kale::Context &ctx) {
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string(
            "Kale Multipart Upload Service Example\n"
            "====================================\n"
            "Upload directory: " + upload_dir + "\n\n"
            "Endpoints:\n"
            "  POST   /api/upload             - Upload multipart/form-data files\n"
            "  GET    /api/uploads            - List uploaded files\n"
            "  GET    /api/uploads/:filename  - Download uploaded file (supports Range)\n"
            "  DELETE /api/uploads/:filename  - Delete an uploaded file\n\n"
            "Sample curl commands:\n"
            "  # Upload single file:\n"
            "  curl -i -F \"file=@examples/assets/hello.txt\" http://127.0.0.1:8080/api/upload\n\n"
            "  # List uploads:\n"
            "  curl -i http://127.0.0.1:8080/api/uploads\n\n"
            "  # Download with Range header:\n"
            "  curl -i -H \"Range: bytes=0-20\" http://127.0.0.1:8080/api/uploads/hello.txt\n"
        );
    });

    return examples::serve(server, options);
}
