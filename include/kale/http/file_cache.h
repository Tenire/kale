#ifndef KALE_FILE_CACHE_H
#define KALE_FILE_CACHE_H

#include <cstddef>
#include <ctime>
#include <memory>
#include <string>

#include "coke/task.h"

namespace kale {

struct FileCacheEntry {
    std::string path_key;
    std::string content;
    std::string etag;
    std::string last_modified;
    std::string mime_type;
    timespec mtime{};
    size_t size{0};
};

struct FileCacheConfig {
    size_t capacity{1024};                    // Maximum retained entry count in LRU table
    size_t max_file_size{4 * 1024 * 1024};    // 4 MB maximum per single file for memory caching
    size_t max_cache_bytes{64 * 1024 * 1024}; // 64 MB maximum payload budget (0 = unlimited)
    bool check_mtime{true};                   // Invalidate and reload if mtime or size changed
};

/**
 * @brief In-memory LRU file cache with stampede protection.
 *
 * Resident cache budget tracks entries in the table, excluding externally pinned entries.
 */
class FileCache {
public:
    explicit FileCache(FileCacheConfig config = {});
    ~FileCache();

    FileCache(const FileCache &) = delete;
    FileCache &operator=(const FileCache &) = delete;
    FileCache(FileCache &&) noexcept;
    FileCache &operator=(FileCache &&) noexcept;

    std::shared_ptr<const FileCacheEntry> get_resident(const std::string &path_key) const;

    /**
     * @brief Asynchronously fetches entry from cache or loads it from open_fd.
     *
     * `open_fd` is borrowed and must remain open until the task completes; caller
     * retains ownership and closes it. Files exceeding max_file_size are not cached.
     * @return Shared cache entry, or nullptr on I/O error.
     */
    coke::Task<std::shared_ptr<const FileCacheEntry>> get_or_load(
        std::string path_key,
        int open_fd,
        size_t file_size,
        timespec mtime,
        std::string mime_type
    );

    void clear();

    size_t size() const noexcept;

    size_t capacity() const noexcept;

    size_t resident_bytes() const noexcept;

    size_t max_cache_bytes() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kale

#endif // KALE_FILE_CACHE_H
