#include "example_server.h"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/http_status.h"
#include "kale/http/form.h"
#include "kale/http/json.h"
#include "kale/http/multipart.h"

int main(int argc, char *argv[]) {
    try {
        // Shared lifecycle initialization must be the first statement in main
        examples::ServerOptions options(argc, argv);

        kale::HttpServer server;

        // ====================================================================
        // 1. POST /submit/contact - URL-Encoded Form Direct Field Inspection
        // ====================================================================
        server.POST("/submit/contact", [](kale::Context &ctx) -> coke::Task<void> {
            std::string_view ct = ctx.content_type();
            if (!ct.starts_with("application/x-www-form-urlencoded")) {
                ctx.set_status(kale::HttpStatus::UnsupportedMediaType);
                ctx.json(kale::Json::Object{
                    {"error", "UNSUPPORTED_MEDIA_TYPE"},
                    {"message", "Expected Content-Type: application/x-www-form-urlencoded"}
                });
                co_return;
            }

            // Inspect required form fields
            if (!kale::has_form(ctx, "name") || !kale::has_form(ctx, "email") || !kale::has_form(ctx, "message")) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "MISSING_FORM_FIELDS"},
                    {"message", "Fields 'name', 'email', and 'message' are required"}
                });
                co_return;
            }

            std::string name = kale::form(ctx, "name");
            std::string email = kale::form(ctx, "email");
            std::string message = kale::form(ctx, "message");

            if (name.empty() || email.empty() || message.empty()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "EMPTY_FIELD_VALUE"},
                    {"message", "Form fields must not be empty"}
                });
                co_return;
            }

            // Inspect all parsed form key-value pairs
            auto all_fields = kale::parse_form(ctx);
            kale::Json::Object fields_json;
            for (const auto &[k, v] : all_fields) {
                fields_json.push_back(k, v);
            }

            kale::Json::Object reply{
                {"status", "received"},
                {"mode", "url_encoded_direct_inspection"},
                {"parsed_field_count", static_cast<int>(all_fields.size())},
                {"fields", std::move(fields_json)}
            };

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(reply);
            co_return;
        });

        // ====================================================================
        // 2. POST /submit/feedback - URL-Encoded Form Validated Ingestion
        // ====================================================================
        server.POST("/submit/feedback", [](kale::Context &ctx) -> coke::Task<void> {
            std::string_view ct = ctx.content_type();
            if (!ct.starts_with("application/x-www-form-urlencoded")) {
                ctx.set_status(kale::HttpStatus::UnsupportedMediaType);
                ctx.json(kale::Json::Object{
                    {"error", "UNSUPPORTED_MEDIA_TYPE"},
                    {"message", "Expected Content-Type: application/x-www-form-urlencoded"}
                });
                co_return;
            }

            auto form_data = kale::parse_form(ctx);

            kale::Json::Array errors;

            // Validate user_name (2..50 chars)
            auto name_it = form_data.find("user_name");
            if (name_it == form_data.end() || name_it->second.size() < 2 || name_it->second.size() > 50) {
                errors.push_back(kale::Json::Object{
                    {"field", "user_name"},
                    {"reason", "user_name length must be between 2 and 50 characters"}
                });
            }

            // Validate email (contains @ and domain dot)
            auto email_it = form_data.find("email");
            if (email_it == form_data.end() || email_it->second.find('@') == std::string::npos ||
                email_it->second.find('.') == std::string::npos) {
                errors.push_back(kale::Json::Object{
                    {"field", "email"},
                    {"reason", "email must be a valid email address"}
                });
            }

            // Validate category enum
            auto cat_it = form_data.find("category");
            static const std::vector<std::string> allowed_cats = {"general", "bug", "feature", "billing"};
            if (cat_it == form_data.end() ||
                std::find(allowed_cats.begin(), allowed_cats.end(), cat_it->second) == allowed_cats.end()) {
                errors.push_back(kale::Json::Object{
                    {"field", "category"},
                    {"reason", "category must be one of: general, bug, feature, billing"}
                });
            }

            // Validate rating (integer 1..5)
            int rating = 0;
            auto rate_it = form_data.find("rating");
            if (rate_it != form_data.end()) {
                auto [ptr, ec] = std::from_chars(rate_it->second.data(),
                                                 rate_it->second.data() + rate_it->second.size(),
                                                 rating);
                if (ec != std::errc{} || rating < 1 || rating > 5) {
                    errors.push_back(kale::Json::Object{
                        {"field", "rating"},
                        {"reason", "rating must be an integer between 1 and 5"}
                    });
                }
            } else {
                errors.push_back(kale::Json::Object{
                    {"field", "rating"},
                    {"reason", "rating is required"}
                });
            }

            // Parse boolean checkbox
            bool subscribed = false;
            auto sub_it = form_data.find("subscribe_newsletter");
            if (sub_it != form_data.end()) {
                subscribed = (sub_it->second == "true" || sub_it->second == "1" ||
                              sub_it->second == "on" || sub_it->second == "yes");
            }

            if (!errors.empty()) {
                ctx.set_status(kale::HttpStatus::UnprocessableEntity);
                ctx.json(kale::Json::Object{
                    {"error", "VALIDATION_FAILED"},
                    {"details", std::move(errors)}
                });
                co_return;
            }

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(kale::Json::Object{
                {"status", "accepted"},
                {"user_name", name_it->second},
                {"email", email_it->second},
                {"category", cat_it->second},
                {"rating", rating},
                {"subscribed", subscribed}
            });
            co_return;
        });

        // ====================================================================
        // 3. POST /submit/application - Multipart Form-Data Text & File Inspection
        // ====================================================================
        server.POST("/submit/application", [](kale::Context &ctx) -> coke::Task<void> {
            std::string_view ct = ctx.content_type();
            if (!ct.starts_with("multipart/form-data")) {
                ctx.set_status(kale::HttpStatus::UnsupportedMediaType);
                ctx.json(kale::Json::Object{
                    {"error", "UNSUPPORTED_MEDIA_TYPE"},
                    {"message", "Expected Content-Type: multipart/form-data with boundary"}
                });
                co_return;
            }

            // Parse and inspect all multipart sections
            std::vector<kale::FormFile> parts = kale::parse_multipart(ctx);
            if (parts.empty()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "EMPTY_MULTIPART"},
                    {"message", "No multipart form parts found or invalid boundary delimiter"}
                });
                co_return;
            }

            // Inspect file part "resume"
            std::optional<kale::FormFile> resume = kale::form_file(ctx, "resume");
            if (!resume) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "MISSING_ATTACHMENT"},
                    {"message", "Multipart form requires a file field named 'resume'"}
                });
                co_return;
            }

            // Validate file properties
            if (resume->filename.empty()) {
                ctx.set_status(kale::HttpStatus::BadRequest);
                ctx.json(kale::Json::Object{
                    {"error", "INVALID_FILE"},
                    {"message", "Attachment 'resume' must contain a non-empty filename"}
                });
                co_return;
            }

            constexpr size_t MAX_FILE_SIZE = 5 * 1024 * 1024; // 5 MiB
            if (resume->data.size() > MAX_FILE_SIZE) {
                ctx.set_status(kale::HttpStatus::PayloadTooLarge);
                ctx.json(kale::Json::Object{
                    {"error", "FILE_TOO_LARGE"},
                    {"message", "Resume file exceeds maximum allowed size of 5 MiB"},
                    {"size_bytes", static_cast<double>(resume->data.size())}
                });
                co_return;
            }

            // Inspect magic bytes (PDF header check)
            bool is_pdf_header = resume->data.size() >= 4 &&
                                 resume->data.substr(0, 4) == "%PDF";

            // Inspect text part "applicant_name"
            std::optional<kale::FormFile> applicant_name = kale::form_file(ctx, "applicant_name");
            std::string name_val = applicant_name.has_value() ? std::string(applicant_name->data) : "";

            // Inspect text part "cover_letter"
            std::optional<kale::FormFile> cover_letter = kale::form_file(ctx, "cover_letter");
            std::string cover_val = cover_letter.has_value() ? std::string(cover_letter->data) : "";

            // Build detailed inspection breakdown of all parts
            kale::Json::Array inspected_parts;
            for (const auto &part : parts) {
                bool is_file = !part.filename.empty();
                kale::Json::Object part_info{
                    {"field_name", part.name},
                    {"is_file", is_file},
                    {"size_bytes", static_cast<double>(part.data.size())}
                };
                if (is_file) {
                    part_info.push_back("filename", part.filename);
                    part_info.push_back("content_type", part.content_type);
                }
                inspected_parts.push_back(std::move(part_info));
            }

            // NOTE: Persisting buffered request payloads via asynchronous file writes belongs to the Uploads scope.
            // See docs/uploads.md for asynchronous file upload and persistence workflows.
            kale::Json::Object reply{
                {"status", "inspected"},
                {"applicant_name", name_val},
                {"has_cover_letter", !cover_val.empty()},
                {"resume", kale::Json::Object{
                    {"filename", resume->filename},
                    {"content_type", resume->content_type},
                    {"size_bytes", static_cast<double>(resume->data.size())},
                    {"valid_pdf_magic", is_pdf_header}
                }},
                {"total_parts_inspected", static_cast<int>(parts.size())},
                {"parts", std::move(inspected_parts)}
            };

            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(reply);
            co_return;
        });

        // ====================================================================
        // 4. GET /health - Basic Health Check Probe
        // ====================================================================
        server.GET("/health", [](kale::Context &ctx) -> coke::Task<void> {
            ctx.set_status(kale::HttpStatus::OK);
            ctx.json(kale::Json::Object{
                {"status", "healthy"},
                {"service", "forms_example"}
            });
            co_return;
        });

        return examples::serve(server, options);
    } catch (const std::exception &e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
