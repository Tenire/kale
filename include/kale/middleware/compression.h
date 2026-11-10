#ifndef KALE_MIDDLEWARE_COMPRESSION_H
#define KALE_MIDDLEWARE_COMPRESSION_H

#include <string>
#include <vector>

#include "kale/core/compress.h"
#include "kale/core/context.h"
#include "kale/core/router.h"

namespace kale {

/**
 * Compression configuration policy for HTTP response bodies.
 */
struct CompressConfig {
    bool enabled{true};
    size_t min_length{1024};
    size_t max_length{0}; // 0 = no maximum limit
    int zstd_level{3};
    int gzip_level{-1};
    std::vector<CompressionEncoding> preferred_encodings{
        CompressionEncoding::Zstd,
        CompressionEncoding::Gzip
    };
    std::vector<std::string> mime_types{
        "text/*",
        "application/json",
        "application/javascript",
        "application/x-javascript",
        "application/xml",
        "application/wasm",
        "image/svg+xml",
        "+json",
        "+xml"
    };

    CompressConfig &set_enabled(bool e) noexcept { enabled = e; return *this; }
    CompressConfig &set_min_length(size_t len) noexcept { min_length = len; return *this; }
    CompressConfig &set_max_length(size_t len) noexcept { max_length = len; return *this; }
    CompressConfig &set_zstd_level(int lvl) noexcept { zstd_level = lvl; return *this; }
    CompressConfig &set_gzip_level(int lvl) noexcept { gzip_level = lvl; return *this; }
    CompressConfig &set_preferred_encodings(std::vector<CompressionEncoding> encs) {
        preferred_encodings = std::move(encs);
        return *this;
    }
    CompressConfig &add_mime_type(std::string mime) {
        mime_types.push_back(std::move(mime));
        return *this;
    }
};

/**
 * Compresses uncommitted response bodies in-place based on config policy.
 * @return true if response was compressed, false if skipped or ineligible.
 * @throws std::bad_alloc on allocation failure.
 */
bool compress_response(Context &ctx, const CompressConfig &config);

/**
 * Install outermost to compress downstream responses before transmission.
 * Skips streamed or already-replied responses.
 * @throws std::bad_alloc if body replacement allocation fails.
 */
Middleware compression(CompressConfig config = {});

} // namespace kale

#endif // KALE_MIDDLEWARE_COMPRESSION_H
