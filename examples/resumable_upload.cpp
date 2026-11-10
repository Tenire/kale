#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "example_server.h"
#include "kale/core/http_server.h"
#include "kale/http/json.h"
#include "kale/http/resumable_upload.h"
#include "kale/security/evp_checksum.h"

int main(int argc, char *argv[]) {
    // 1. Process server options and set up signal mask before worker threads start
    examples::ServerOptions options(argc, argv);

    // 2. Resolve target storage directory for resumable parts
    std::string upload_dir = "build/example-uploads/resumable";
    if (const char *env_dir = std::getenv("KALE_EXAMPLE_UPLOAD_DIR")) {
        upload_dir = std::string(env_dir) + "/resumable";
    }

    std::error_code ec;
    std::filesystem::create_directories(upload_dir, ec);
    if (ec) {
        std::cerr << "[resumable_upload] Error: Cannot create storage directory '"
                  << upload_dir << "': " << ec.message() << "\n";
        return 1;
    }

    // 3. Configure HTTP server parameters
    // In-memory coordination & persistence limits:
    // Uploads in Kale are bounded buffered per-request payloads with async direct-offset
    // persistence across requests; there is no single-request ingress streaming.
    // ResumableUploadCoordinator manages active session descriptors (file descriptors and part
    // bitsets) in-memory. Each session sets the target file's logical length using
    // ftruncate, and incoming parts write directly to their respective disk offsets via
    // non-blocking coke::pwrite. If the server process terminates before complete_session,
    // active in-memory session tracking is discarded.
    kale::HttpServerParams params;
    params.set_keep_alive_timeout(30000)
          .set_request_size_limit(32 * 1024 * 1024); // 32MB max request limit

    kale::HttpServer server(params);

    // Shared coordinator instance with reference-counted lifetime across route lambdas
    auto coordinator = std::make_shared<kale::ResumableUploadCoordinator>();

    // 4. Create new resumable upload session
    server.POST("/api/resumable/sessions", [upload_dir, coordinator](kale::Context &ctx) -> coke::Task<void> {
        if (!ctx.is_json()) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Expected Content-Type: application/json";
            ctx.json(err);
            co_return;
        }

        kale::Json j;
        try {
            j = kale::Json::parse(ctx.req_body());
        } catch (...) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Malformed JSON payload";
            ctx.json(err);
            co_return;
        }

        if (!j.is_object()) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Expected JSON object";
            ctx.json(err);
            co_return;
        }

        std::string upload_id = (j.has("upload_id") && j["upload_id"].is_string()) ? j["upload_id"].get<std::string>() : "";
        std::string filename = (j.has("filename") && j["filename"].is_string()) ? j["filename"].get<std::string>() : "";
        double total_size_dbl = (j.has("total_size") && j["total_size"].is_number()) ? j["total_size"].get<double>() : 0.0;
        double part_size_dbl = (j.has("part_size") && j["part_size"].is_number()) ? j["part_size"].get<double>() : 0.0;

        if (upload_id.empty() || filename.empty() || total_size_dbl <= 0 || part_size_dbl <= 0) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Missing or invalid parameters: upload_id, filename, total_size, part_size";
            ctx.json(err);
            co_return;
        }

        // Prevent path traversal
        if (filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos ||
            filename == "." || filename == "..") {
            ctx.set_status(kale::HttpStatus::Forbidden);
            kale::Json err;
            err["error"] = "Invalid filename";
            ctx.json(err);
            co_return;
        }

        uint64_t total_size = static_cast<uint64_t>(total_size_dbl);
        size_t part_size = static_cast<size_t>(part_size_dbl);

        // Bounded sanity limits (max 64MB for this example)
        if (total_size > 64 * 1024 * 1024 || part_size < 1024 || part_size > total_size) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Size out of allowable bounds (max total: 64MB, min part: 1KB)";
            ctx.json(err);
            co_return;
        }

        std::filesystem::path target = std::filesystem::path(upload_dir) / (upload_id + "_" + filename);

        int init_ret = co_await coordinator->init_session(upload_id, target.string(), total_size, part_size);
        if (init_ret != 0) {
            ctx.set_status(kale::HttpStatus::Conflict);
            kale::Json err;
            err["error"] = "Session could not be initialized (already exists or disk error)";
            err["code"] = init_ret;
            ctx.json(err);
            co_return;
        }

        auto info_opt = coordinator->get_session(upload_id);
        if (!info_opt) {
            ctx.set_status(kale::HttpStatus::InternalServerError);
            kale::Json err;
            err["error"] = "Internal error retrieving initialized session";
            ctx.json(err);
            co_return;
        }

        kale::Json out;
        out["status"] = "created";
        out["upload_id"] = upload_id;
        out["target_path"] = target.string();
        out["total_size"] = static_cast<double>(info_opt->total_size);
        out["part_size"] = static_cast<double>(info_opt->part_size);
        out["total_parts"] = static_cast<double>(info_opt->total_parts);
        ctx.set_status(kale::HttpStatus::Created);
        ctx.json(out);
    });

    // 5. Upload part with direct offset write and optional SHA-256 verifier
    server.PUT("/api/resumable/sessions/:id/parts/:part_num", [coordinator](kale::Context &ctx) -> coke::Task<void> {
        std::string upload_id(ctx.param("id"));
        std::string part_num_str(ctx.param("part_num"));

        uint32_t part_number = 0;
        try {
            part_number = static_cast<uint32_t>(std::stoul(part_num_str));
        } catch (...) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Invalid part_num: must be a positive 1-based integer";
            ctx.json(err);
            co_return;
        }

        if (part_number == 0) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Part number must be >= 1";
            ctx.json(err);
            co_return;
        }

        std::string expected_sha256 = ctx.header("X-Checksum-SHA256");
        std::string_view part_data = ctx.req_body();

        if (part_data.empty()) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Empty part payload received";
            ctx.json(err);
            co_return;
        }

        // Direct offset write via coke::pwrite and OpenSSL EVP SHA-256 verifier
        kale::EvpChecksum verifier("SHA256");
        int write_ret = co_await coordinator->write_part(
            upload_id,
            part_number,
            part_data,
            expected_sha256,
            expected_sha256.empty() ? nullptr : &verifier
        );

        if (write_ret == 0) {
            kale::Json ok_json;
            ok_json["status"] = "uploaded";
            ok_json["upload_id"] = upload_id;
            ok_json["part_number"] = static_cast<double>(part_number);
            ok_json["bytes_received"] = static_cast<double>(part_data.size());
            ctx.json(ok_json);
        } else if (write_ret == -ENOENT) {
            ctx.set_status(kale::HttpStatus::NotFound);
            kale::Json err;
            err["error"] = "Upload session not found: " + upload_id;
            ctx.json(err);
        } else if (write_ret == -EBADMSG) {
            ctx.set_status(kale::HttpStatus::UnprocessableEntity);
            kale::Json err;
            err["error"] = "SHA-256 checksum mismatch for part " + std::to_string(part_number);
            ctx.json(err);
        } else if (write_ret == -EINVAL) {
            ctx.set_status(kale::HttpStatus::BadRequest);
            kale::Json err;
            err["error"] = "Invalid part number or boundary exceeds total parts";
            ctx.json(err);
        } else {
            ctx.set_status(kale::HttpStatus::InternalServerError);
            kale::Json err;
            err["error"] = "Failed to write part to disk";
            err["code"] = write_ret;
            ctx.json(err);
        }
    });

    // 6. Query session status
    server.GET("/api/resumable/sessions/:id", [coordinator](kale::Context &ctx) {
        std::string upload_id(ctx.param("id"));
        auto info_opt = coordinator->get_session(upload_id);
        if (!info_opt) {
            ctx.set_status(kale::HttpStatus::NotFound);
            kale::Json err;
            err["error"] = "Upload session not found: " + upload_id;
            ctx.json(err);
            return;
        }

        kale::Json out;
        out["upload_id"] = info_opt->upload_id;
        out["target_path"] = info_opt->target_path;
        out["total_size"] = static_cast<double>(info_opt->total_size);
        out["part_size"] = static_cast<double>(info_opt->part_size);
        out["total_parts"] = static_cast<double>(info_opt->total_parts);
        out["is_completed"] = info_opt->is_completed;

        kale::Json completed_arr = kale::Json::parse("[]");
        for (uint32_t p : info_opt->completed_parts) {
            completed_arr.push_back(static_cast<double>(p));
        }
        out["completed_parts"] = completed_arr;

        ctx.json(out);
    });

    // 7. Finalize session, verify full-file SHA-256 checksum, flush to disk
    server.POST("/api/resumable/sessions/:id/complete", [coordinator](kale::Context &ctx) -> coke::Task<void> {
        std::string upload_id(ctx.param("id"));

        kale::EvpChecksum final_chk("SHA256");
        int complete_ret = co_await coordinator->complete_session(upload_id, &final_chk);

        if (complete_ret == 0) {
            kale::Json ok_json;
            ok_json["status"] = "completed";
            ok_json["upload_id"] = upload_id;
            ok_json["sha256"] = final_chk.hex_digest();
            ctx.json(ok_json);
        } else if (complete_ret == -ENOENT) {
            ctx.set_status(kale::HttpStatus::NotFound);
            kale::Json err;
            err["error"] = "Upload session not found: " + upload_id;
            ctx.json(err);
        } else if (complete_ret == -EBUSY) {
            ctx.set_status(kale::HttpStatus::Conflict);
            kale::Json err;
            err["error"] = "Cannot complete: pending parts have not been uploaded yet";
            ctx.json(err);
        } else {
            ctx.set_status(kale::HttpStatus::InternalServerError);
            kale::Json err;
            err["error"] = "Failed to finalize session";
            err["code"] = complete_ret;
            ctx.json(err);
        }
    });

    // 8. Abort and clean up session
    server.DELETE("/api/resumable/sessions/:id", [coordinator](kale::Context &ctx) -> coke::Task<void> {
        std::string upload_id(ctx.param("id"));
        int ret = co_await coordinator->abort_session(upload_id);
        if (ret == 0) {
            kale::Json ok_json;
            ok_json["status"] = "aborted";
            ok_json["upload_id"] = upload_id;
            ctx.json(ok_json);
        } else if (ret == -ENOENT) {
            ctx.set_status(kale::HttpStatus::NotFound);
            kale::Json err;
            err["error"] = "Upload session not found: " + upload_id;
            ctx.json(err);
        } else {
            ctx.set_status(kale::HttpStatus::InternalServerError);
            kale::Json err;
            err["error"] = "Failed to abort session";
            err["code"] = ret;
            ctx.json(err);
        }
    });

    // 9. Root guide with curl workflow
    server.GET("/", [upload_dir](kale::Context &ctx) {
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string(
            "Kale Resumable Chunk Upload Service\n"
            "===================================\n"
            "Storage directory: " + upload_dir + "\n\n"
            "Endpoints:\n"
            "  POST   /api/resumable/sessions                   - Initialize session (ftruncate logical sizing)\n"
            "  PUT    /api/resumable/sessions/:id/parts/:part   - Write part direct to offset (with SHA256 check)\n"
            "  GET    /api/resumable/sessions/:id               - Query session status & completed parts\n"
            "  POST   /api/resumable/sessions/:id/complete      - Finalize session & compute full file SHA256\n"
            "  DELETE /api/resumable/sessions/:id               - Abort session and unlink file\n"
        );
    });

    return examples::serve(server, options);
}
