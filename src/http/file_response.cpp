#include "kale/http/file_response.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"

#include "kale/core/context.h"
#include "coke/fileio.h"
#include "coke/go.h"
#include "http/file_util.h"
#include "http/mime_type.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace kale {
namespace {

struct OpenedFile {
    int fd{-1};
    int error{0};
    size_t size{0};
};

std::string_view trim_range(std::string_view input) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) input.remove_prefix(1);
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t')) input.remove_suffix(1);
    return input;
}

bool decimal(std::string_view text, size_t &value) {
    if (text.empty()) return false;
    value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        size_t digit = static_cast<size_t>(c - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10) value = std::numeric_limits<size_t>::max();
        else value = value * 10 + digit;
    }
    return true;
}

coke::Task<int> send_file_impl(Context &ctx, std::string filename, size_t start, size_t end,
                               std::string range_spec, bool parse_range) {
    if (ctx.is_replied()) throw std::logic_error("Cannot send a file after replying");
    if (filename.find('\0') != std::string::npos) {
        reply_error(ctx, HttpError::bad_request("Bad Request"));
        co_return -EINVAL;
    }

    const auto opened = co_await coke::go([&filename] {
        OpenedFile file;
        do { file.fd = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK); }
        while (file.fd < 0 && errno == EINTR);
        if (file.fd < 0) { file.error = errno; return file; }
        struct stat st{};
        if (::fstat(file.fd, &st) != 0) file.error = errno;
        else if (!S_ISREG(st.st_mode) || st.st_size < 0) file.error = ENOENT;
        else file.size = static_cast<size_t>(st.st_size);
        if (file.error) { ::close(file.fd); file.fd = -1; }
        return file;
    });

    if (opened.error) {
        const int error = opened.error;
        if (error == ENOENT || error == ENOTDIR) reply_error(ctx, HttpError::not_found("Not Found"));
        else if (error == EACCES || error == EPERM) reply_error(ctx, HttpError::forbidden("Forbidden"));
        else reply_error(ctx, HttpError::internal_error("Internal Server Error"));
        co_return -error;
    }

    FileDescriptor fd(opened.fd);
    const size_t size = opened.size;
    bool partial = start != 0 || end != static_cast<size_t>(-1);
    bool valid = true;
    end = std::min(end, size);
    std::string_view range = trim_range(range_spec);

    if (parse_range && ctx.method() == HttpMethod::GET && range.starts_with("bytes=")) {
        partial = true;
        auto spec = range.substr(6);
        const auto dash = spec.find('-');
        valid = dash != std::string_view::npos && spec.find(',') == std::string_view::npos;
        if (valid) {
            auto first = spec.substr(0, dash), last = spec.substr(dash + 1);
            size_t value = 0;
            if (first.empty()) {
                valid = decimal(last, value) && value != 0;
                start = size - std::min(value, size);
                end = size;
            } else {
                valid = decimal(first, start);
                if (last.empty()) end = size;
                else {
                    valid = valid && decimal(last, value) && value >= start;
                    end = value >= size ? size : value + 1;
                }
            }
        }
    }

    if (partial && (size == 0 || start >= size || end <= start)) valid = false;
    ctx.raw_resp().clear_output_body();
    ctx.erase_resp_header("Content-Encoding");
    ctx.erase_resp_header("Transfer-Encoding");
    ctx.erase_resp_header("Content-Range");

    if (!valid) {
        ctx.set_status(HttpStatus::RangeNotSatisfiable);
        ctx.set_header("Content-Range", "bytes */" + std::to_string(size));
        ctx.set_header("Content-Length", "0");
        co_await fd.close();
        co_return -ERANGE;
    }

    const size_t length = end - start;
    ctx.set_status(partial ? HttpStatus::PartialContent : HttpStatus::OK);
    ctx.set_header("Accept-Ranges", "bytes");
    ctx.set_header("Content-Length", std::to_string(length));
    ctx.set_content_type(get_mime_type(filename));
    if (partial) ctx.set_header("Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end - 1) + "/" + std::to_string(size));

    if (ctx.method() == HttpMethod::HEAD || length == 0) {
        co_await fd.close();
        co_return 0;
    }

    std::array<char, 64 * 1024> buffer;
    size_t offset = start;
    while (offset < end) {
        auto result = co_await coke::pread(fd.get(), buffer.data(), std::min(buffer.size(), end - offset), static_cast<off_t>(offset));
        if (result.state != 0 || result.nbytes <= 0) {
            const int error = result.error ? result.error : EIO;
            co_await fd.close();
            if (ctx.is_stream()) ctx.abort_stream();
            else {
                reply_error(ctx, HttpError::internal_error("Internal Server Error"));
            }
            co_return -error;
        }
        if (!co_await ctx.send_chunk(std::string_view(buffer.data(), static_cast<size_t>(result.nbytes)))) {
            co_await fd.close();
            ctx.abort_stream();
            co_return -EPIPE;
        }
        offset += static_cast<size_t>(result.nbytes);
    }

    const bool ok = co_await ctx.end_chunk();
    co_await fd.close();
    co_return ok ? 0 : -EIO;
}

} // namespace

coke::Task<int> send_file(Context &ctx, std::string_view path, size_t start, size_t end) {
    return send_file_impl(ctx, std::string(path), start, end, {}, false);
}

coke::Task<int> send_file_range(Context &ctx, std::string_view path, std::string_view range_header) {
    return send_file_impl(ctx, std::string(path), 0, static_cast<size_t>(-1), std::string(range_header), true);
}

} // namespace kale
