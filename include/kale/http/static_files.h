#ifndef KALE_STATIC_FILES_H
#define KALE_STATIC_FILES_H

#include <memory>
#include <string>
#include <string_view>

#include "coke/task.h"

#include "kale/core/router.h"
#include "kale/http/file_cache.h"

namespace kale {

struct StaticFilesConfig {
    std::string root_dir;
    std::string url_prefix{"/static"};
    std::string index_file{"index.html"};
    bool enable_cache{true};
    FileCacheConfig cache_config{};
    // When false (default), symlinks are rejected with 403 Forbidden.
    bool allow_symlinks{false};
    std::string default_content_type{"application/octet-stream"};
    std::string cache_control;
};

Handler static_files(std::string root_dir);

Handler static_files(StaticFilesConfig config);

/**
 * @brief Creates a static file handler borrowing an external FileCache.
 * The cache must outlive the handler and in-flight requests.
 */
Handler static_files(StaticFilesConfig config, FileCache &cache);

Handler static_files(StaticFilesConfig config, std::shared_ptr<FileCache> cache);

struct CachedFileOptions {
    int status_code{200};
    std::string cache_control{};
    std::string default_content_type{"application/octet-stream"};
};

/**
 * @brief Process-local default FileCache used by the two-argument serve_cached_file overload.
 */
FileCache &default_file_cache();

/**
 * @brief Serves a single file through a FileCache with ETag, Last-Modified,
 * conditional 304/412, Range, and pre-compressed variant negotiation.
 */
coke::Task<void> serve_cached_file(
    Context &ctx,
    std::string_view file_path,
    CachedFileOptions options = {});

coke::Task<void> serve_cached_file(
    Context &ctx,
    std::string_view file_path,
    FileCache &cache,
    CachedFileOptions options = {});
} // namespace kale

#endif // KALE_STATIC_FILES_H
