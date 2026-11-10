#ifndef KALE_STATIC_FILES_H
#define KALE_STATIC_FILES_H

#include <memory>
#include <string>

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
} // namespace kale

#endif // KALE_STATIC_FILES_H
