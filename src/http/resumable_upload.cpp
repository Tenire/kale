#include "kale/http/resumable_upload.h"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <unistd.h>

#include "coke/fileio.h"
#include "coke/go.h"

namespace kale {

namespace {

enum class SessionState {
    Initializing,
    Active,
    Completing,
    Aborting,
    CleanupRequired
};

struct InternalSession {
    UploadSessionInfo info;
    int fd{-1};
    SessionState state{SessionState::Initializing};
    std::set<uint32_t> completed_set;
    std::set<uint32_t> active_parts;
    size_t active_writers{0};

    ~InternalSession() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

} // namespace

struct ResumableUploadCoordinator::Impl {
    std::mutex mtx;
    std::map<std::string, std::unique_ptr<InternalSession>> sessions;
    std::set<std::string> reserved_paths;
};

ResumableUploadCoordinator::ResumableUploadCoordinator()
    : impl_(std::make_unique<Impl>()) {}

ResumableUploadCoordinator::~ResumableUploadCoordinator() = default;

coke::Task<int> ResumableUploadCoordinator::init_session(
    std::string upload_id,
    std::string target_path,
    uint64_t total_size,
    size_t part_size
) {
    try {
        if (upload_id.empty() || target_path.empty() || total_size == 0 || part_size == 0) {
            co_return -EINVAL;
        }
        if (upload_id.find('\0') != std::string::npos || target_path.find('\0') != std::string::npos) {
            co_return -EINVAL;
        }
        if (total_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
            co_return -EFBIG;
        }

        uint64_t total_parts_64 = (total_size / part_size) + ((total_size % part_size != 0) ? 1 : 0);
        if (total_parts_64 > std::numeric_limits<uint32_t>::max() || total_parts_64 == 0) {
            co_return -EINVAL;
        }
        uint32_t total_parts = static_cast<uint32_t>(total_parts_64);

        std::filesystem::path p(target_path);
        std::string normal_path = p.lexically_normal().string();

        struct InitRollbackGuard {
            ResumableUploadCoordinator::Impl *impl{nullptr};
            std::string_view id;
            std::string_view path;
            int fd{-1};
            bool committed{false};

            ~InitRollbackGuard() {
                if (!committed && impl) {
                    if (fd >= 0) {
                        ::close(fd);
                    }
                    std::lock_guard<std::mutex> lock(impl->mtx);
                    if (!path.empty()) {
                        impl->reserved_paths.erase(std::string(path));
                    }
                    if (!id.empty()) {
                        impl->sessions.erase(std::string(id));
                    }
                }
            }
        } guard{impl_.get(), upload_id, normal_path, -1, false};

        auto sess = std::make_unique<InternalSession>();
        sess->info.upload_id = upload_id;
        sess->info.target_path = target_path;
        sess->info.total_size = total_size;
        sess->info.part_size = part_size;
        sess->info.total_parts = total_parts;
        sess->info.is_completed = false;
        sess->fd = -1;
        sess->state = SessionState::Initializing;

        InternalSession *raw_sess = sess.get();

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            if (impl_->sessions.find(upload_id) != impl_->sessions.end()) {
                guard.committed = true;
                co_return -EEXIST;
            }
            if (impl_->reserved_paths.find(normal_path) != impl_->reserved_paths.end()) {
                guard.committed = true;
                co_return -EEXIST;
            }

            auto [path_it, p_ok] = impl_->reserved_paths.insert(normal_path);
            try {
                impl_->sessions.emplace(upload_id, std::move(sess));
            } catch (...) {
                impl_->reserved_paths.erase(path_it);
                guard.committed = true;
                throw;
            }
        }

        struct SetupResult {
            int fd{-1};
            int error{0};
        };

        SetupResult setup_res = co_await coke::go([target_path, total_size] {
            SetupResult res;
            try {
                std::error_code ec;
                std::filesystem::path tp(target_path);
                bool existed = std::filesystem::exists(tp, ec);

                if (tp.has_parent_path()) {
                    std::filesystem::create_directories(tp.parent_path(), ec);
                    if (ec) {
                        res.error = -ec.value();
                        return res;
                    }
                }

                int opened;
                do {
                    opened = ::open(target_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                } while (opened < 0 && errno == EINTR);
                if (opened < 0) {
                    res.error = -errno;
                    return res;
                }

                if (::ftruncate(opened, static_cast<off_t>(total_size)) != 0) {
                    int err = errno;
                    ::close(opened);
                    if (!existed) {
                        std::filesystem::remove(tp, ec);
                    }
                    res.error = -err;
                    return res;
                }

                res.fd = opened;
                return res;
            } catch (const std::bad_alloc &) {
                res.error = -ENOMEM;
                return res;
            } catch (const std::exception &) {
                res.error = -EIO;
                return res;
            } catch (...) {
                res.error = -EIO;
                return res;
            }
        });

        if (setup_res.error != 0) {
            co_return setup_res.error;
        }

        guard.fd = setup_res.fd;

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            raw_sess->fd = setup_res.fd;
            raw_sess->state = SessionState::Active;
        }

        guard.committed = true;
        co_return 0;
    } catch (const std::bad_alloc &) {
        co_return -ENOMEM;
    } catch (const std::exception &) {
        co_return -EIO;
    } catch (...) {
        co_return -EIO;
    }
}

coke::Task<int> ResumableUploadCoordinator::write_part(
    std::string upload_id,
    uint32_t part_number,
    std::string_view data,
    std::string expected_checksum,
    ChecksumVerifier *verifier
) {
    try {
        InternalSession *sess = nullptr;
        int target_fd = -1;
        off_t write_offset = 0;

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            auto it = impl_->sessions.find(upload_id);
            if (it == impl_->sessions.end()) {
                co_return -ENOENT;
            }
            sess = it->second.get();

            if (sess->state != SessionState::Active) {
                co_return -EBUSY;
            }
            if (part_number == 0 || part_number > sess->info.total_parts) {
                co_return -EINVAL;
            }
            if (sess->completed_set.find(part_number) != sess->completed_set.end()) {
                co_return -EEXIST; // Duplicate completed part
            }
            if (sess->active_parts.find(part_number) != sess->active_parts.end()) {
                co_return -EBUSY; // Active write collision
            }

            if (part_number < sess->info.total_parts) {
                if (data.size() != sess->info.part_size) {
                    co_return -EINVAL;
                }
            } else {
                uint64_t expected_final_len = sess->info.total_size -
                    static_cast<uint64_t>(sess->info.total_parts - 1) * sess->info.part_size;
                if (data.size() != expected_final_len) {
                    co_return -EINVAL;
                }
            }

            uint64_t offset_64 = static_cast<uint64_t>(part_number - 1) * static_cast<uint64_t>(sess->info.part_size);
            if (offset_64 > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
                co_return -EFBIG;
            }
            write_offset = static_cast<off_t>(offset_64);
            target_fd = sess->fd;
            if (target_fd < 0) {
                co_return -EBADF;
            }

            sess->active_parts.insert(part_number);
            sess->active_writers++;
        }

        struct ActiveGuard {
            ResumableUploadCoordinator::Impl *impl{nullptr};
            InternalSession *s{nullptr};
            uint32_t p{0};
            bool committed{false};

            ~ActiveGuard() {
                if (!committed && impl && s) {
                    std::lock_guard<std::mutex> lock(impl->mtx);
                    s->active_parts.erase(p);
                    s->active_writers--;
                }
            }
        } guard{impl_.get(), sess, part_number, false};

        if (!expected_checksum.empty()) {
            if (!verifier) {
                co_return -EINVAL;
            }
            try {
                verifier->reset();
                verifier->update(data);
                std::string digest = verifier->hex_digest();
                if (digest.empty()) {
                    co_return -EIO;
                }
                if (digest != expected_checksum) {
                    co_return -EBADMSG;
                }
            } catch (const std::bad_alloc &) {
                co_return -ENOMEM;
            } catch (const std::exception &) {
                co_return -EIO;
            } catch (...) {
                co_return -EIO;
            }
        }

        constexpr size_t block_size = 64 * 1024;
        size_t written = 0;
        int write_error = 0;
        while (written < data.size()) {
            size_t count = std::min(block_size, data.size() - written);
            off_t cur_offset = write_offset + static_cast<off_t>(written);

            coke::FileResult res = co_await coke::pwrite(target_fd, data.data() + written, count, cur_offset);
            if (res.state != 0 || res.nbytes <= 0) {
                write_error = (res.state == 0) ? -EIO : -res.error;
                break;
            }
            written += static_cast<size_t>(res.nbytes);
        }

        if (write_error != 0) {
            co_return write_error;
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            sess->completed_set.insert(part_number);
            if (sess->completed_set.size() == sess->info.total_parts) {
                sess->info.is_completed = true;
            }
            sess->active_parts.erase(part_number);
            sess->active_writers--;
            guard.committed = true;
        }

        co_return 0;
    } catch (const std::bad_alloc &) {
        co_return -ENOMEM;
    } catch (const std::exception &) {
        co_return -EIO;
    } catch (...) {
        co_return -EIO;
    }
}

std::optional<UploadSessionInfo> ResumableUploadCoordinator::get_session(const std::string &upload_id) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->sessions.find(upload_id);
    if (it == impl_->sessions.end()) {
        return std::nullopt;
    }
    UploadSessionInfo info = it->second->info;
    info.completed_parts.assign(it->second->completed_set.begin(), it->second->completed_set.end());
    return info;
}

coke::Task<int> ResumableUploadCoordinator::complete_session(
    std::string upload_id,
    ChecksumVerifier *final_checksum
) {
    try {
        InternalSession *sess = nullptr;
        int fd = -1;
        uint64_t total_size = 0;
        std::string normal_path;

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            auto it = impl_->sessions.find(upload_id);
            if (it == impl_->sessions.end()) {
                co_return -ENOENT;
            }
            sess = it->second.get();

            if (sess->state != SessionState::Active) {
                co_return -EBUSY;
            }
            if (!sess->info.is_completed) {
                co_return -EBUSY; // Missing parts
            }
            if (sess->active_writers > 0) {
                co_return -EBUSY; // Active writes in flight
            }

            normal_path = std::filesystem::path(sess->info.target_path).lexically_normal().string();
            sess->state = SessionState::Completing;
            fd = sess->fd;
            total_size = sess->info.total_size;
        }

        struct CompleteRollbackGuard {
            ResumableUploadCoordinator::Impl *impl{nullptr};
            InternalSession *s{nullptr};
            bool committed{false};

            ~CompleteRollbackGuard() {
                if (!committed && impl && s) {
                    std::lock_guard<std::mutex> lock(impl->mtx);
                    s->state = SessionState::Active;
                }
            }
        } rollback{impl_.get(), sess, false};

        int fs_err = co_await coke::go([fd, total_size, final_checksum] {
            try {
                if (fd < 0) return -EBADF;
                if (::fsync(fd) != 0) {
                    return -errno;
                }
                if (final_checksum) {
                    final_checksum->reset();
                    constexpr size_t buf_size = 64 * 1024;
                    std::vector<char> buf(buf_size);
                    off_t cur = 0;
                    while (cur < static_cast<off_t>(total_size)) {
                        off_t remaining = static_cast<off_t>(total_size) - cur;
                        size_t to_read = std::min<size_t>(buf_size, static_cast<size_t>(remaining));

                        ssize_t n;
                        do {
                            n = ::pread(fd, buf.data(), to_read, cur);
                        } while (n < 0 && errno == EINTR);
                        if (n < 0) {
                            return -errno;
                        }
                        if (n == 0) {
                            return -EIO;
                        }
                        final_checksum->update(buf.data(), static_cast<size_t>(n));
                        cur += n;
                    }
                    std::string digest = final_checksum->hex_digest();
                    if (digest.empty()) {
                        return -EIO;
                    }
                }
                return 0;
            } catch (const std::bad_alloc &) {
                return -ENOMEM;
            } catch (const std::exception &) {
                return -EIO;
            } catch (...) {
                return -EIO;
            }
        });

        if (fs_err != 0) {
            co_return fs_err;
        }

        int close_err = co_await coke::go([fd] {
            return ::close(fd) == 0 ? 0 : -errno;
        });

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            sess->fd = -1;
        }

        if (close_err != 0) {
            co_return close_err;
        }

        rollback.committed = true;

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->reserved_paths.erase(normal_path);
            impl_->sessions.erase(upload_id);
        }

        co_return 0;
    } catch (const std::bad_alloc &) {
        co_return -ENOMEM;
    } catch (const std::exception &) {
        co_return -EIO;
    } catch (...) {
        co_return -EIO;
    }
}

coke::Task<int> ResumableUploadCoordinator::abort_session(std::string upload_id) {
    try {
        InternalSession *sess = nullptr;
        std::string normal_path;
        std::string target_path;
        int fd = -1;

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            auto it = impl_->sessions.find(upload_id);
            if (it == impl_->sessions.end()) {
                co_return -ENOENT;
            }
            sess = it->second.get();

            if (sess->state != SessionState::Active && sess->state != SessionState::CleanupRequired) {
                co_return -EBUSY;
            }
            if (sess->active_writers > 0) {
                co_return -EBUSY; // Active writes in flight
            }

            normal_path = std::filesystem::path(sess->info.target_path).lexically_normal().string();
            target_path = sess->info.target_path;

            sess->state = SessionState::Aborting;
            fd = sess->fd;
            sess->fd = -1;
        }

        int abort_err = co_await coke::go([fd, target_path] {
            try {
                int err = 0;
                if (fd >= 0) {
                    if (::close(fd) != 0) {
                        err = -errno;
                    }
                }
                if (!target_path.empty()) {
                    std::error_code ec;
                    if (std::filesystem::exists(target_path, ec)) {
                        std::filesystem::remove(target_path, ec);
                        if (ec && err == 0) {
                            err = -ec.value();
                        }
                    }
                }
                return err;
            } catch (const std::bad_alloc &) {
                return -ENOMEM;
            } catch (const std::exception &) {
                return -EIO;
            } catch (...) {
                return -EIO;
            }
        });

        if (abort_err != 0) {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            sess->state = SessionState::CleanupRequired;
            co_return abort_err;
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->reserved_paths.erase(normal_path);
            impl_->sessions.erase(upload_id);
        }

        co_return 0;
    } catch (const std::bad_alloc &) {
        co_return -ENOMEM;
    } catch (const std::exception &) {
        co_return -EIO;
    } catch (...) {
        co_return -EIO;
    }
}

coke::Task<int> ResumableUploadCoordinator::clear() {
    try {
        std::vector<std::pair<int, std::string>> detached_items;
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            for (const auto &pair : impl_->sessions) {
                const auto &sess = pair.second;
                if ((sess->state != SessionState::Active && sess->state != SessionState::CleanupRequired) ||
                    sess->active_writers > 0) {
                    co_return -EBUSY;
                }
            }

            detached_items.reserve(impl_->sessions.size());
            for (auto &pair : impl_->sessions) {
                pair.second->state = SessionState::Aborting;
                int fd = pair.second->fd;
                pair.second->fd = -1;
                std::string np = std::filesystem::path(pair.second->info.target_path).lexically_normal().string();
                detached_items.emplace_back(fd, np);
            }
            impl_->sessions.clear();
        }

        int clear_err = 0;
        for (const auto &item : detached_items) {
            int fd = item.first;
            if (fd >= 0) {
                int err = co_await coke::go([fd] {
                    return ::close(fd) == 0 ? 0 : -errno;
                });
                if (err != 0 && clear_err == 0) {
                    clear_err = err;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            for (const auto &item : detached_items) {
                impl_->reserved_paths.erase(item.second);
            }
        }

        co_return clear_err;
    } catch (const std::bad_alloc &) {
        co_return -ENOMEM;
    } catch (const std::exception &) {
        co_return -EIO;
    } catch (...) {
        co_return -EIO;
    }
}

} // namespace kale
