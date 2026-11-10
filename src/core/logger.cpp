#include "kale/core/logger.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace kale {

void Logger::log(LogLevel level, const LogContext &ctx, std::string_view message) const noexcept {
    if (!enabled(level)) {
        return;
    }

    if (sink_) {
        try {
            sink_(level, ctx, message);
        } catch (...) {
            // User sink exceptions must never escape
        }
        return;
    }

    // Default console output: format into stack buffer char buf[1024]
    // single best-effort POSIX ::write(STDERR_FILENO, ...)
    char buf[1024];
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time_t_now, &tm_buf);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    int written = snprintf(
        buf, sizeof(buf),
        "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%-5.*s]",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms),
        static_cast<int>(level_to_string(level).size()), level_to_string(level).data()
    );

    if (written < 0) return;
    size_t offset = static_cast<size_t>(written);

    if (!ctx.component.empty() && offset + 3 + ctx.component.size() < sizeof(buf)) {
        written = snprintf(buf + offset, sizeof(buf) - offset, " [%.*s]",
                           static_cast<int>(ctx.component.size()), ctx.component.data());
        if (written > 0) offset += static_cast<size_t>(written);
    }

    if ((!ctx.method.empty() || !ctx.path.empty() || ctx.status_code != 0) && offset + 16 < sizeof(buf)) {
        if (ctx.status_code != 0) {
            written = snprintf(buf + offset, sizeof(buf) - offset, " [%.*s %.*s %d]",
                               static_cast<int>(ctx.method.size()), ctx.method.data(),
                               static_cast<int>(ctx.path.size()), ctx.path.data(),
                               ctx.status_code);
        } else {
            written = snprintf(buf + offset, sizeof(buf) - offset, " [%.*s %.*s]",
                               static_cast<int>(ctx.method.size()), ctx.method.data(),
                               static_cast<int>(ctx.path.size()), ctx.path.data());
        }
        if (written > 0) offset += static_cast<size_t>(written);
    }

    if (offset + 2 < sizeof(buf)) {
        buf[offset++] = ' ';
        size_t avail = sizeof(buf) - offset - 2; // leave room for \n\0
        size_t to_copy = message.size() < avail ? message.size() : avail;
        std::memcpy(buf + offset, message.data(), to_copy);
        offset += to_copy;
    }

    if (offset >= sizeof(buf) - 1) {
        offset = sizeof(buf) - 2;
    }
    buf[offset++] = '\n';
    buf[offset] = '\0';

    ssize_t result = ::write(STDERR_FILENO, buf, offset);
    (void)result;
}

} // namespace kale
