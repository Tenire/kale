#include "kale/middleware/compression.h"

#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "kale/core/compress.h"
#include "kale/core/http_util.h"

namespace kale {

namespace {

inline std::string_view trim_ows(std::string_view str) noexcept {
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) {
        str.remove_prefix(1);
    }
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) {
        str.remove_suffix(1);
    }
    return str;
}

inline bool case_insensitive_starts_with(std::string_view str, std::string_view prefix) noexcept {
    if (str.size() < prefix.size()) {
        return false;
    }
    return case_insensitive_equal(str.substr(0, prefix.size()), prefix);
}

inline bool case_insensitive_ends_with(std::string_view str, std::string_view suffix) noexcept {
    if (str.size() < suffix.size()) {
        return false;
    }
    return case_insensitive_equal(str.substr(str.size() - suffix.size()), suffix);
}

inline bool vary_contains_accept_encoding(std::string_view vary) noexcept {
    if (vary == "*") {
        return true;
    }
    size_t start = 0;
    while (start < vary.size()) {
        const size_t comma = vary.find(',', start);
        std::string_view token = (comma == std::string_view::npos)
            ? vary.substr(start)
            : vary.substr(start, comma - start);
        token = trim_ows(token);
        if (token == "*" || case_insensitive_equal(token, "Accept-Encoding")) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

} // namespace

bool compress_response(Context &ctx, const CompressConfig &config) {
    if (!config.enabled || ctx.is_replied() || ctx.is_stream()) {
        return false;
    }
    if (ctx.has_resp_header("Content-Encoding") || ctx.has_resp_header("Content-Range")) {
        return false;
    }

    const int current_status = ctx.status_code();
    if (current_status == 206 || current_status == 204 || current_status == 304 || current_status < 200) {
        return false;
    }

    const size_t body_size = ctx.resp_body_size();
    if (body_size < config.min_length || (config.max_length != 0 && body_size > config.max_length)) {
        return false;
    }

    auto mime = ctx.resp_header("Content-Type");
    mime = trim_ows(mime.substr(0, mime.find(';')));
    const auto mime_matches = [mime](std::string_view pattern) {
        if (pattern == "*") return true;
        if (pattern.ends_with('*')) {
            return case_insensitive_starts_with(mime, pattern.substr(0, pattern.size() - 1));
        }
        if (pattern.starts_with('+')) {
            return case_insensitive_ends_with(mime, pattern);
        }
        return case_insensitive_equal(mime, pattern);
    };

    bool allowed_mime = false;
    for (const auto &pattern : config.mime_types) {
        if (mime_matches(pattern)) {
            allowed_mime = true;
            break;
        }
    }
    if (!allowed_mime) {
        return false;
    }

    if (!ctx.has_header("Accept-Encoding")) {
        return false;
    }
    const std::string accept_encoding = ctx.header("Accept-Encoding");
    const auto encoding = Compress::negotiate(accept_encoding, config.preferred_encodings);
    if (encoding == CompressionEncoding::None) {
        return false;
    }

    const void *blocks[2];
    size_t block_sizes[2];
    const size_t nblocks = ctx.get_resp_body_blocks(blocks, block_sizes, 2);

    std::string merged_storage;
    std::string_view uncompressed_body;
    if (nblocks == 1) {
        uncompressed_body = std::string_view(static_cast<const char *>(blocks[0]), block_sizes[0]);
    } else if (nblocks > 1) {
        if (!ctx.get_resp_body_merged(merged_storage)) {
            return false;
        }
        uncompressed_body = merged_storage;
    } else {
        return false;
    }

    std::string compressed;
    const bool ok = (encoding == CompressionEncoding::Zstd)
        ? Compress::zstd(uncompressed_body, compressed, config.zstd_level)
        : (encoding == CompressionEncoding::Gzip)
            ? Compress::gzip(uncompressed_body, compressed, config.gzip_level)
            : false;
    if (!ok) {
        return false;
    }

    // Replace native output body with compressed payload.
    // On native allocation failure, fail explicitly via std::bad_alloc rather than returning false.
    ctx.raw_resp().clear_output_body();
    if (!ctx.raw_resp().append_output_body(compressed.data(), compressed.size())) {
        throw std::bad_alloc();
    }
    ctx.erase_resp_header("Content-Length");
    ctx.set_header("Content-Encoding", std::string(encoding_to_string(encoding)));
    const auto vary = ctx.resp_header("Vary");
    if (vary.empty()) {
        ctx.set_header("Vary", "Accept-Encoding");
    } else if (!vary_contains_accept_encoding(vary)) {
        ctx.set_header("Vary", std::string(vary) + ", Accept-Encoding");
    }
    return true;
}

Middleware compression(CompressConfig config) {
    return [config = std::move(config)](Context &ctx, Next next) -> coke::Task<void> {
        co_await next();
        if (!ctx.is_stream() && !ctx.is_replied()) {
            compress_response(ctx, config);
        }
    };
}

} // namespace kale
