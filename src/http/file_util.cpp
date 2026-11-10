#include "http/file_util.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <utility>
#include <unistd.h>

#include "coke/fileio.h"
#include "coke/go.h"

namespace kale {

namespace {

coke::Task<int> close_descriptor(int fd) {
    co_return co_await coke::go([fd] {
        // On Linux close releases the descriptor even if it reports EINTR.
        // Retrying could close a descriptor subsequently reused by another task.
        return ::close(fd) == 0 ? 0 : -errno;
    });
}

} // namespace


FileDescriptor::~FileDescriptor() {
    if (fd_ >= 0) {
        coke::detach(close_descriptor(std::exchange(fd_, -1)));
    }
}

coke::Task<int> FileDescriptor::close() {
    const int fd = std::exchange(fd_, -1);
    co_return fd >= 0 ? co_await close_descriptor(fd) : 0;
}


coke::Task<int> FileUtil::write_file(
    const std::string &filepath,
    std::string_view data,
    bool append
) {
    if (filepath.find('\0') != std::string::npos) {
        co_return -EINVAL;
    }

    const int opened = co_await coke::go([&filepath, append] {
        std::error_code ec;
        const std::filesystem::path path(filepath);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                return -ec.value();
            }
        }

        // O_NONBLOCK prevents a special file (e.g. FIFO) from tying up a worker
        // indefinitely. Ordinary files are unaffected.
        const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK |
                          (append ? O_APPEND : O_TRUNC);
        int fd;
        do {
            fd = ::open(filepath.c_str(), flags, 0644);
        } while (fd < 0 && errno == EINTR);
        return fd >= 0 ? fd : -errno;
    });
    if (opened < 0) {
        co_return opened;
    }
    FileDescriptor fd(opened);

    constexpr size_t block_size = 64 * 1024;
    size_t written = 0;
    int error = 0;
    while (written < data.size()) {
        const size_t count = std::min(block_size, data.size() - written);
        auto result = co_await coke::pwrite(opened, data.data() + written, count,
                                            static_cast<off_t>(written));
        if (result.state != 0 || result.nbytes <= 0) {
            error = result.state == 0 ? -EIO : -result.error;
            break;
        }
        written += static_cast<size_t>(result.nbytes);
    }

    const int close_error = co_await fd.close();
    co_return error != 0 ? error : close_error;
}


} // namespace kale
