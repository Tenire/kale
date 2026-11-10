#ifndef KALE_UTIL_FILE_UTIL_H
#define KALE_UTIL_FILE_UTIL_H

#include <string>
#include <string_view>

#include "coke/task.h"

namespace kale {


// Owns a descriptor across suspensions. Even exceptional cleanup is dispatched
// to the file executor rather than closing a file on a network worker.
class FileDescriptor {
public:
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
    ~FileDescriptor();
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;

    int get() const noexcept { return fd_; }
    coke::Task<int> close();

private:
    int fd_;
};


class FileUtil {
public:
    FileUtil() = delete;

    /**
     * Writes data to filepath, creating parent directories if needed.
     * The caller must keep filepath and data alive until the task completes.
     * @return 0 on success, or negative errno on failure.
     */
    static coke::Task<int> write_file(
        const std::string &filepath,
        std::string_view data,
        bool append = false
    );

};

} // namespace kale

#endif // KALE_UTIL_FILE_UTIL_H
