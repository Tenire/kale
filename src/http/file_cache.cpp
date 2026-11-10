#include "kale/http/file_cache.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <list>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "coke/condition.h"
#include "coke/fileio.h"

namespace kale {

namespace {

std::string format_http_date(time_t sec) {
    struct tm tm_buf{};
    gmtime_r(&sec, &tm_buf);
    char buf[64];
    size_t len = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return std::string(buf, len);
}

std::string compute_strong_etag(std::string_view content) {
    // 64-bit FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : content) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << '"' << std::hex << std::setfill('0') << std::setw(16) << hash << '"';
    return oss.str();
}

} // namespace

class FileCache::Impl {
public:
    explicit Impl(FileCacheConfig config) : config_(config) {}

    ~Impl() {
        clear();
    }

    std::shared_ptr<const FileCacheEntry> get_resident(const std::string &path_key) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = map_.find(path_key);
        if (it != map_.end()) {
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return *(it->second);
        }
        return nullptr;
    }

    coke::Task<std::shared_ptr<const FileCacheEntry>> get_or_load(
        std::string path_key,
        int open_fd,
        size_t file_size,
        timespec mtime,
        std::string mime_type
    ) {
        std::unique_lock<std::mutex> lock(mtx_);

        auto it = map_.find(path_key);
        if (it != map_.end()) {
            auto entry = *(it->second);
            if (config_.check_mtime) {
                if (entry->mtime.tv_sec == mtime.tv_sec &&
                    entry->mtime.tv_nsec == mtime.tv_nsec &&
                    entry->size == file_size) {
                    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
                    co_return entry;
                }
                resident_bytes_ -= entry->content.size();
                lru_list_.erase(it->second);
                map_.erase(it);
            } else {
                lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
                co_return entry;
            }
        }

        if (file_size > config_.max_file_size) {
            co_return nullptr;
        }

        // Await in-flight load if already active
        auto in_flight_it = in_flight_.find(path_key);
        if (in_flight_it != in_flight_.end()) {
            auto state = in_flight_it->second;
            while (!state->done) {
                co_await state->cond.wait(lock);
            }
            co_return state->entry;
        }

        auto state = std::make_shared<InFlightLoad>();
        in_flight_[path_key] = state;
        lock.unlock();

        try {
            std::string buffer;
            buffer.resize(file_size);
            size_t bytes_read = 0;
            int read_error = 0;

            constexpr size_t chunk_size = 64 * 1024;
            while (bytes_read < file_size) {
                size_t to_read = std::min(chunk_size, file_size - bytes_read);
                auto res = co_await coke::pread(open_fd, buffer.data() + bytes_read, to_read, static_cast<off_t>(bytes_read));
                if (res.state != 0 || res.nbytes <= 0) {
                    read_error = res.state != 0 ? res.state : -EIO;
                    break;
                }
                bytes_read += static_cast<size_t>(res.nbytes);
            }

            lock.lock();

            if (read_error != 0 || bytes_read != file_size) {
                state->done = true;
                state->entry = nullptr;
                in_flight_.erase(path_key);
                lock.unlock();
                state->cond.notify_all();
                co_return nullptr;
            }

            auto entry = std::make_shared<FileCacheEntry>();
            entry->path_key = path_key;
            entry->content = std::move(buffer);
            entry->etag = compute_strong_etag(entry->content);
            entry->last_modified = format_http_date(mtime.tv_sec);
            entry->mime_type = std::move(mime_type);
            entry->mtime = mtime;
            entry->size = file_size;

            const size_t entry_bytes = entry->content.size();

            // Enforce LRU eviction bounds
            while (!lru_list_.empty() && (
                (config_.capacity > 0 && map_.size() >= config_.capacity) ||
                (config_.max_cache_bytes > 0 && resident_bytes_ + entry_bytes > config_.max_cache_bytes)
            )) {
                auto oldest = lru_list_.back();
                resident_bytes_ -= oldest->content.size();
                map_.erase(oldest->path_key);
                lru_list_.pop_back();
            }

            if (config_.max_cache_bytes == 0 || entry_bytes <= config_.max_cache_bytes) {
                lru_list_.push_front(entry);
                try {
                    map_[path_key] = lru_list_.begin();
                } catch (...) {
                    lru_list_.pop_front();
                    throw;
                }
                resident_bytes_ += entry_bytes;
            }
            auto result_entry = entry;
            state->done = true;
            state->entry = entry;
            in_flight_.erase(path_key);
            lock.unlock();
            state->cond.notify_all();

            co_return result_entry;
        } catch (...) {
            if (!lock.owns_lock()) {
                lock.lock();
            }
            state->done = true;
            state->entry = nullptr;
            in_flight_.erase(path_key);
            lock.unlock();
            state->cond.notify_all();
            throw;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        map_.clear();
        lru_list_.clear();
        resident_bytes_ = 0;
    }

    size_t size() noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return map_.size();
    }

    size_t capacity() const noexcept {
        return config_.capacity;
    }

    size_t resident_bytes() noexcept {
        std::lock_guard<std::mutex> lock(mtx_);
        return resident_bytes_;
    }

    size_t max_cache_bytes() const noexcept {
        return config_.max_cache_bytes;
    }

private:
    struct InFlightLoad {
        coke::Condition cond;
        bool done{false};
        std::shared_ptr<const FileCacheEntry> entry;
    };

    FileCacheConfig config_;
    std::mutex mtx_;
    std::list<std::shared_ptr<const FileCacheEntry>> lru_list_;
    std::unordered_map<std::string, std::list<std::shared_ptr<const FileCacheEntry>>::iterator> map_;
    std::unordered_map<std::string, std::shared_ptr<InFlightLoad>> in_flight_;
    size_t resident_bytes_{0};
};

FileCache::FileCache(FileCacheConfig config)
    : impl_(std::make_unique<Impl>(config)) {}

FileCache::~FileCache() = default;
FileCache::FileCache(FileCache &&) noexcept = default;
FileCache &FileCache::operator=(FileCache &&) noexcept = default;

std::shared_ptr<const FileCacheEntry> FileCache::get_resident(const std::string &path_key) const {
    return impl_ ? impl_->get_resident(path_key) : nullptr;
}

coke::Task<std::shared_ptr<const FileCacheEntry>> FileCache::get_or_load(
    std::string path_key,
    int open_fd,
    size_t file_size,
    timespec mtime,
    std::string mime_type
) {
    if (!impl_) co_return nullptr;
    co_return co_await impl_->get_or_load(
        std::move(path_key), open_fd, file_size, mtime, std::move(mime_type)
    );
}

void FileCache::clear() {
    if (impl_) impl_->clear();
}

size_t FileCache::size() const noexcept {
    return impl_ ? impl_->size() : 0;
}

size_t FileCache::capacity() const noexcept {
    return impl_ ? impl_->capacity() : 0;
}

size_t FileCache::resident_bytes() const noexcept {
    return impl_ ? impl_->resident_bytes() : 0;
}

size_t FileCache::max_cache_bytes() const noexcept {
    return impl_ ? impl_->max_cache_bytes() : 0;
}

} // namespace kale
