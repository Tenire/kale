#include "kale/http/static_files.h"
#include "kale/core/error_response.h"
#include "kale/core/http_error.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "coke/fileio.h"
#include "coke/go.h"
#include "kale/core/context.h"
#include "kale/core/router.h"
#include "kale/http/file_cache.h"
#include "kale/core/http_method.h"
#include "kale/core/http_status.h"
#include "kale/core/http_util.h"
#include "http/file_util.h"
#include "http/mime_type.h"

namespace kale {

namespace {

std::string format_http_date(time_t sec) {
    struct tm tm_buf{};
    gmtime_r(&sec, &tm_buf);
    char buf[64];
    size_t len = strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
    return std::string(buf, len);
}

bool parse_http_date(std::string_view str, time_t &out_time) {
    while (!str.empty() && (str.front() == ' ' || str.front() == '\t')) str.remove_prefix(1);
    while (!str.empty() && (str.back() == ' ' || str.back() == '\t')) str.remove_suffix(1);
    if (str.empty()) return false;

    std::string s(str);
    struct tm tm_buf{};
    if (strptime(s.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf) != nullptr) {
        out_time = timegm(&tm_buf);
        return true;
    }
    if (strptime(s.c_str(), "%A, %d-%b-%y %H:%M:%S GMT", &tm_buf) != nullptr) {
        out_time = timegm(&tm_buf);
        return true;
    }
    if (strptime(s.c_str(), "%a %b %e %H:%M:%S %Y", &tm_buf) != nullptr) {
        out_time = timegm(&tm_buf);
        return true;
    }
    return false;
}

bool strong_etag_match(std::string_view req_tag, std::string_view res_tag) {
    if (req_tag.starts_with("W/") || req_tag.starts_with("w/")) return false;
    if (res_tag.starts_with("W/") || res_tag.starts_with("w/")) return false;
    return req_tag == res_tag;
}

std::string_view extract_opaque_tag(std::string_view tag) {
    if (tag.starts_with("W/") || tag.starts_with("w/")) {
        tag.remove_prefix(2);
    }
    return tag;
}

bool weak_etag_match(std::string_view req_tag, std::string_view res_tag) {
    return extract_opaque_tag(req_tag) == extract_opaque_tag(res_tag);
}

bool if_match_satisfied(std::string_view header, std::string_view res_tag) {
    while (!header.empty()) {
        while (!header.empty() && (header.front() == ' ' || header.front() == '\t' || header.front() == ',')) {
            header.remove_prefix(1);
        }
        if (header.empty()) break;
        if (header.front() == '*') return true;
        size_t end = 0;
        if (header.front() == '"') {
            end = header.find('"', 1);
            if (end != std::string_view::npos) end += 1;
            else end = header.size();
        } else {
            end = header.find(',');
            if (end == std::string_view::npos) end = header.size();
        }
        std::string_view tag = header.substr(0, end);
        header.remove_prefix(end);
        while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.remove_prefix(1);
        while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
        if (strong_etag_match(tag, res_tag)) return true;
    }
    return false;
}

bool if_none_match_matches(std::string_view header, std::string_view res_tag) {
    while (!header.empty()) {
        while (!header.empty() && (header.front() == ' ' || header.front() == '\t' || header.front() == ',')) {
            header.remove_prefix(1);
        }
        if (header.empty()) break;
        if (header.front() == '*') return true;
        size_t end = 0;
        if (header.starts_with("W/\"") || header.starts_with("w/\"")) {
            end = header.find('"', 3);
            if (end != std::string_view::npos) end += 1;
            else end = header.size();
        } else if (header.front() == '"') {
            end = header.find('"', 1);
            if (end != std::string_view::npos) end += 1;
            else end = header.size();
        } else {
            end = header.find(',');
            if (end == std::string_view::npos) end = header.size();
        }
        std::string_view tag = header.substr(0, end);
        header.remove_prefix(end);
        while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t')) tag.remove_prefix(1);
        while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t')) tag.remove_suffix(1);
        if (weak_etag_match(tag, res_tag)) return true;
    }
    return false;
}

struct RangeSpec {
    bool has_range{false};
    bool valid{false};
    size_t start{0};
    size_t end{0}; // exclusive end
};

RangeSpec parse_byte_range(std::string_view range_header, size_t total_size) {
    RangeSpec spec;
    while (!range_header.empty() && (range_header.front() == ' ' || range_header.front() == '\t')) {
        range_header.remove_prefix(1);
    }
    while (!range_header.empty() && (range_header.back() == ' ' || range_header.back() == '\t')) {
        range_header.remove_suffix(1);
    }
    if (!range_header.starts_with("bytes=")) {
        return spec;
    }
    spec.has_range = true;
    std::string_view r = range_header.substr(6);
    if (r.find(',') != std::string_view::npos) {
        spec.valid = false;
        return spec;
    }
    size_t dash = r.find('-');
    if (dash == std::string_view::npos) {
        spec.valid = false;
        return spec;
    }
    std::string_view first = r.substr(0, dash);
    std::string_view last = r.substr(dash + 1);

    auto parse_u64 = [](std::string_view s, size_t &val) -> bool {
        if (s.empty()) return false;
        val = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            val = val * 10 + static_cast<size_t>(c - '0');
        }
        return true;
    };

    if (first.empty()) {
        size_t suffix_len = 0;
        if (!parse_u64(last, suffix_len) || suffix_len == 0) {
            spec.valid = false;
            return spec;
        }
        if (total_size == 0) {
            spec.valid = false;
            return spec;
        }
        spec.start = total_size > suffix_len ? total_size - suffix_len : 0;
        spec.end = total_size;
        spec.valid = true;
        return spec;
    } else {
        size_t start_val = 0;
        if (!parse_u64(first, start_val)) {
            spec.valid = false;
            return spec;
        }
        if (last.empty()) {
            if (start_val >= total_size) {
                spec.valid = false;
                return spec;
            }
            spec.start = start_val;
            spec.end = total_size;
            spec.valid = true;
            return spec;
        } else {
            size_t end_val = 0;
            if (!parse_u64(last, end_val) || end_val < start_val) {
                spec.valid = false;
                return spec;
            }
            if (start_val >= total_size) {
                spec.valid = false;
                return spec;
            }
            spec.start = start_val;
            spec.end = std::min(end_val + 1, total_size);
            spec.valid = true;
            return spec;
        }
    }
}

bool should_satisfy_range(std::string_view if_range_header, std::string_view res_etag, timespec mtime) {
    if (if_range_header.empty()) return true;
    while (!if_range_header.empty() && (if_range_header.front() == ' ' || if_range_header.front() == '\t')) {
        if_range_header.remove_prefix(1);
    }
    while (!if_range_header.empty() && (if_range_header.back() == ' ' || if_range_header.back() == '\t')) {
        if_range_header.remove_suffix(1);
    }
    if (if_range_header.starts_with("W/") || if_range_header.starts_with("w/")) {
        return false;
    }
    if (if_range_header.front() == '"' && if_range_header.back() == '"') {
        return strong_etag_match(if_range_header, res_etag);
    }
    time_t if_range_date = 0;
    if (parse_http_date(if_range_header, if_range_date)) {
        return mtime.tv_sec <= if_range_date;
    }
    return false;
}

coke::Task<void> stream_descriptor(Context &ctx, FileDescriptor &fd, size_t start, size_t end) {
    if (start >= end) {
        co_await fd.close();
        co_return;
    }
    std::array<char, 64 * 1024> buffer;
    size_t offset = start;
    while (offset < end) {
        size_t to_read = std::min(buffer.size(), end - offset);
        auto res = co_await coke::pread(fd.get(), buffer.data(), to_read, static_cast<off_t>(offset));
        if (res.state != 0 || res.nbytes <= 0) {
            co_await fd.close();
            if (ctx.is_stream()) ctx.abort_stream();
            co_return;
        }
        if (!co_await ctx.send_chunk(std::string_view(buffer.data(), static_cast<size_t>(res.nbytes)))) {
            co_await fd.close();
            ctx.abort_stream();
            co_return;
        }
        offset += static_cast<size_t>(res.nbytes);
    }
    co_await ctx.end_chunk();
    co_await fd.close();
}

struct ResolveResult {
    enum class Status {
        File,
        Redirect,
        NotFound,
        Forbidden,
        Error
    } status{Status::NotFound};
    int fd{-1};
    struct stat st{};
    std::string key;
};

Handler make_static_files_handler(StaticFilesConfig config,
                                  std::shared_ptr<FileCache> cache_owner,
                                  FileCache *cache) {
    if (config.root_dir.empty()) {
        config.root_dir = ".";
    }
    if (config.root_dir.find('\0') != std::string::npos) {
        throw std::invalid_argument("kale::static_files: root_dir contains null character");
    }
    if (config.index_file.find('\0') != std::string::npos ||
        config.index_file.find('/') != std::string::npos) {
        throw std::invalid_argument("kale::static_files: index_file must be a simple file name");
    }

    return [config = std::move(config),
            cache_owner = std::move(cache_owner),
            cache](Context &ctx) -> coke::Task<void> {
        (void)cache_owner;
        if (ctx.method() != HttpMethod::GET && ctx.method() != HttpMethod::HEAD) {
            reply_error(ctx, HttpError::method_not_allowed("GET, HEAD", "Method Not Allowed"));
            co_return;
        }

        std::string_view raw_path = ctx.path();
        if (raw_path.find('\0') != std::string_view::npos) {
            reply_error(ctx, HttpError::bad_request("Bad Request: Null character in path"));
            co_return;
        }
        for (size_t i = 0; i + 2 < raw_path.size(); ++i) {
            if (raw_path[i] == '%' && (raw_path[i+1] == '0' || raw_path[i+1] == '0') && (raw_path[i+2] == '0')) {
                reply_error(ctx, HttpError::bad_request("Bad Request: Null character in path"));
                co_return;
            }
            if (raw_path[i] == '%' && raw_path[i+1] == '2' && (raw_path[i+2] == 'f' || raw_path[i+2] == 'F')) {
                reply_error(ctx, HttpError::bad_request("Bad Request: Encoded slash not allowed"));
                co_return;
            }
        }

        std::string rel_path;
        if (ctx.has_param("filepath")) {
            rel_path = ctx.param("filepath");
        } else if (ctx.has_param("*")) {
            rel_path = ctx.param("*");
        } else {
            std::string_view p = ctx.path();
            if (!config.url_prefix.empty() && p.starts_with(config.url_prefix)) {
                p.remove_prefix(config.url_prefix.size());
            }
            rel_path = path_percent_decode(p);
        }

        if (rel_path.find('\0') != std::string::npos) {
            reply_error(ctx, HttpError::bad_request("Bad Request: Null character in path"));
            co_return;
        }

        std::string cleaned = PathUtil::clean_path(rel_path);
        if (cleaned == ".." || cleaned.starts_with("../")) {
            reply_error(ctx, HttpError::forbidden("Forbidden: Path traversal detected"));
            co_return;
        }

        std::vector<std::string> comps;
        {
            size_t start = 0;
            while (start < rel_path.size()) {
                while (start < rel_path.size() && rel_path[start] == '/') ++start;
                if (start >= rel_path.size()) break;
                size_t end = rel_path.find('/', start);
                if (end == std::string::npos) end = rel_path.size();
                std::string comp = rel_path.substr(start, end - start);
                if (comp == "..") {
                    reply_error(ctx, HttpError::forbidden("Forbidden: Path traversal detected"));
                    co_return;
                }
                if (comp != "." && !comp.empty()) {
                    comps.push_back(std::move(comp));
                }
                start = end;
            }
        }

        bool ends_with_slash = (!rel_path.empty() && rel_path.back() == '/') ||
                               (!raw_path.empty() && raw_path.back() == '/');

        auto resolved = co_await coke::go([&]() -> ResolveResult {
            ResolveResult res;
            const int nofollow_flag = config.allow_symlinks ? 0 : O_NOFOLLOW;
            int root_dfd = ::open(config.root_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (root_dfd < 0) {
                res.status = (errno == ENOENT) ? ResolveResult::Status::NotFound : ResolveResult::Status::Forbidden;
                return res;
            }

            if (comps.empty()) {
                if (!ends_with_slash) {
                    ::close(root_dfd);
                    res.status = ResolveResult::Status::Redirect;
                    return res;
                }
                if (config.index_file.empty()) {
                    ::close(root_dfd);
                    res.status = ResolveResult::Status::NotFound;
                    return res;
                }
                int idx_fd = ::openat(root_dfd, config.index_file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | nofollow_flag);
                ::close(root_dfd);
                if (idx_fd < 0) {
                    res.status = (errno == ELOOP || errno == EACCES) ? ResolveResult::Status::Forbidden : ResolveResult::Status::NotFound;
                    return res;
                }
                if (::fstat(idx_fd, &res.st) != 0 || !S_ISREG(res.st.st_mode)) {
                    ::close(idx_fd);
                    res.status = ResolveResult::Status::Forbidden;
                    return res;
                }
                res.status = ResolveResult::Status::File;
                res.fd = idx_fd;
                res.key = config.index_file;
                return res;
            }

            int cur_dfd = root_dfd;
            for (size_t i = 0; i + 1 < comps.size(); ++i) {
                int next_dfd = ::openat(cur_dfd, comps[i].c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | nofollow_flag);
                if (cur_dfd != root_dfd) ::close(cur_dfd);
                if (next_dfd < 0) {
                    ::close(root_dfd);
                    if (errno == ELOOP || errno == ENOTDIR || errno == EACCES || errno == EPERM) {
                        res.status = ResolveResult::Status::Forbidden;
                    } else if (errno == ENOENT) {
                        res.status = ResolveResult::Status::NotFound;
                    } else {
                        res.status = ResolveResult::Status::Error;
                    }
                    return res;
                }
                cur_dfd = next_dfd;
            }

            const std::string &leaf = comps.back();
            if (ends_with_slash) {
                int dir_fd = ::openat(cur_dfd, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | nofollow_flag);
                if (cur_dfd != root_dfd) ::close(cur_dfd);
                ::close(root_dfd);
                if (dir_fd < 0) {
                    if (errno == ELOOP || errno == EACCES || errno == EPERM) {
                        res.status = ResolveResult::Status::Forbidden;
                    } else {
                        res.status = ResolveResult::Status::NotFound;
                    }
                    return res;
                }
                if (config.index_file.empty()) {
                    ::close(dir_fd);
                    res.status = ResolveResult::Status::NotFound;
                    return res;
                }
                int idx_fd = ::openat(dir_fd, config.index_file.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | nofollow_flag);
                ::close(dir_fd);
                if (idx_fd < 0) {
                    res.status = (errno == ELOOP || errno == EACCES) ? ResolveResult::Status::Forbidden : ResolveResult::Status::NotFound;
                    return res;
                }
                if (::fstat(idx_fd, &res.st) != 0 || !S_ISREG(res.st.st_mode)) {
                    ::close(idx_fd);
                    res.status = ResolveResult::Status::Forbidden;
                    return res;
                }
                res.status = ResolveResult::Status::File;
                res.fd = idx_fd;
                res.key = rel_path;
                if (!res.key.empty() && res.key.back() != '/') res.key.push_back('/');
                res.key += config.index_file;
                return res;
            }

            int leaf_fd = ::openat(cur_dfd, leaf.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | nofollow_flag);
            if (leaf_fd < 0) {
                if (errno == EISDIR) {
                    if (cur_dfd != root_dfd) ::close(cur_dfd);
                    ::close(root_dfd);
                    res.status = ResolveResult::Status::Redirect;
                    return res;
                }
                if (errno == ELOOP || errno == EACCES || errno == EPERM) {
                    if (cur_dfd != root_dfd) ::close(cur_dfd);
                    ::close(root_dfd);
                    res.status = ResolveResult::Status::Forbidden;
                    return res;
                }
                int dir_fd = ::openat(cur_dfd, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | nofollow_flag);
                if (cur_dfd != root_dfd) ::close(cur_dfd);
                ::close(root_dfd);
                if (dir_fd >= 0) {
                    ::close(dir_fd);
                    res.status = ResolveResult::Status::Redirect;
                    return res;
                }
                res.status = (errno == ENOENT) ? ResolveResult::Status::NotFound : ResolveResult::Status::Forbidden;
                return res;
            }

            if (cur_dfd != root_dfd) ::close(cur_dfd);
            ::close(root_dfd);

            if (::fstat(leaf_fd, &res.st) != 0) {
                ::close(leaf_fd);
                res.status = ResolveResult::Status::Error;
                return res;
            }

            if (S_ISDIR(res.st.st_mode)) {
                ::close(leaf_fd);
                res.status = ResolveResult::Status::Redirect;
                return res;
            }

            if (!S_ISREG(res.st.st_mode)) {
                ::close(leaf_fd);
                res.status = ResolveResult::Status::Forbidden;
                return res;
            }

            res.status = ResolveResult::Status::File;
            res.fd = leaf_fd;
            res.key = rel_path;
            return res;
        });

        if (resolved.status == ResolveResult::Status::Redirect) {
            std::string location;
            std::string_view p = ctx.path();
            location.append(p);
            location.push_back('/');
            const char *uri = ctx.raw_req().get_request_uri();
            if (uri) {
                const char *q = std::strchr(uri, '?');
                if (q) location.append(q);
            }
            for (char c : location) {
                if (c == '\r' || c == '\n') {
                    ctx.set_status(HttpStatus::BadRequest).string("Bad Request");
                    co_return;
                }
            }
            ctx.set_status(HttpStatus::MovedPermanently);
            ctx.set_header("Location", location);
            ctx.set_header("Content-Length", "0");
            co_return;
        }

        if (resolved.status == ResolveResult::Status::NotFound) {
            reply_error(ctx, HttpError::not_found("Not Found"));
            co_return;
        }

        if (resolved.status == ResolveResult::Status::Forbidden) {
            reply_error(ctx, HttpError::forbidden("Forbidden"));
            co_return;
        }

        if (resolved.status == ResolveResult::Status::Error) {
            reply_error(ctx, HttpError::internal_error("Internal Server Error"));
            co_return;
        }

        FileDescriptor fd(resolved.fd);
        std::string mime_type = std::string(get_mime_type(resolved.key));
        if (mime_type.empty()) {
            mime_type = config.default_content_type;
        }

        std::shared_ptr<const FileCacheEntry> entry = nullptr;
        if (config.enable_cache && cache) {
            if (static_cast<size_t>(resolved.st.st_size) <= config.cache_config.max_file_size) {
                std::string_view rel_key = resolved.key;
                while (!rel_key.empty() && rel_key.front() == '/') {
                    rel_key.remove_prefix(1);
                }
                std::string cache_key = (std::filesystem::path(config.root_dir) / rel_key).string();
                entry = co_await cache->get_or_load(
                    std::move(cache_key), fd.get(), static_cast<size_t>(resolved.st.st_size),
                    resolved.st.st_mtim, mime_type
                );
            }
        }

        std::string etag;
        std::string last_modified;
        size_t file_size = 0;
        timespec mtime{};

        if (entry != nullptr) {
            etag = entry->etag;
            last_modified = entry->last_modified;
            file_size = entry->size;
            mtime = entry->mtime;
            co_await fd.close();
        } else {
            std::ostringstream oss;
            oss << "W/\"" << std::hex << resolved.st.st_size << '-'
                << resolved.st.st_mtim.tv_sec << '-'
                << resolved.st.st_mtim.tv_nsec << '"';
            etag = oss.str();
            last_modified = format_http_date(resolved.st.st_mtim.tv_sec);
            file_size = static_cast<size_t>(resolved.st.st_size);
            mtime = resolved.st.st_mtim;
        }

        // RFC 7232 precedence: If-Match -> If-Unmodified-Since -> If-None-Match -> If-Modified-Since
        if (ctx.has_header("If-Match")) {
            if (!if_match_satisfied(ctx.header("If-Match"), etag)) {
                if (fd.get() >= 0) co_await fd.close();
                ctx.set_status(412).string("Precondition Failed");
                co_return;
            }
        } else if (ctx.has_header("If-Unmodified-Since")) {
            time_t unmod_date = 0;
            if (parse_http_date(ctx.header("If-Unmodified-Since"), unmod_date)) {
                if (mtime.tv_sec > unmod_date) {
                    if (fd.get() >= 0) co_await fd.close();
                    ctx.set_status(412).string("Precondition Failed");
                    co_return;
                }
            }
        }

        if (ctx.has_header("If-None-Match")) {
            if (if_none_match_matches(ctx.header("If-None-Match"), etag)) {
                if (fd.get() >= 0) co_await fd.close();
                ctx.set_status(HttpStatus::NotModified);
                ctx.set_header("ETag", etag);
                ctx.set_header("Last-Modified", last_modified);
                if (!config.cache_control.empty()) ctx.set_header("Cache-Control", config.cache_control);
                ctx.set_header("Content-Length", "0");
                co_return;
            }
        } else if (ctx.has_header("If-Modified-Since")) {
            time_t mod_date = 0;
            if (parse_http_date(ctx.header("If-Modified-Since"), mod_date)) {
                if (mtime.tv_sec <= mod_date) {
                    if (fd.get() >= 0) co_await fd.close();
                    ctx.set_status(HttpStatus::NotModified);
                    ctx.set_header("ETag", etag);
                    ctx.set_header("Last-Modified", last_modified);
                    if (!config.cache_control.empty()) ctx.set_header("Cache-Control", config.cache_control);
                    ctx.set_header("Content-Length", "0");
                    co_return;
                }
            }
        }

        ctx.set_header("Accept-Ranges", "bytes");
        ctx.set_header("ETag", etag);
        ctx.set_header("Last-Modified", last_modified);
        if (!config.cache_control.empty()) ctx.set_header("Cache-Control", config.cache_control);

        bool handle_range = false;
        RangeSpec rspec;
        if (ctx.method() == HttpMethod::GET && ctx.has_header("Range")) {
            std::string r_hdr = ctx.header("Range");
            std::string if_r_hdr = ctx.has_header("If-Range") ? ctx.header("If-Range") : "";
            if (should_satisfy_range(if_r_hdr, etag, mtime)) {
                rspec = parse_byte_range(r_hdr, file_size);
                if (rspec.has_range) {
                    handle_range = true;
                }
            }
        }

        if (handle_range) {
            if (!rspec.valid) {
                if (fd.get() >= 0) co_await fd.close();
                ctx.set_status(HttpStatus::RangeNotSatisfiable);
                ctx.set_header("Content-Range", "bytes */" + std::to_string(file_size));
                ctx.set_header("Content-Length", "0");
                co_return;
            }

            size_t start = rspec.start;
            size_t end = rspec.end;
            size_t length = end - start;

            ctx.set_status(HttpStatus::PartialContent);
            ctx.set_header("Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end - 1) + "/" + std::to_string(file_size));
            ctx.set_header("Content-Length", std::to_string(length));
            ctx.set_content_type(mime_type);

            if (ctx.method() == HttpMethod::HEAD) {
                if (fd.get() >= 0) co_await fd.close();
                co_return;
            }

            if (entry != nullptr) {
                ctx.body(std::string_view(entry->content).substr(start, length));
                co_return;
            }

            co_await stream_descriptor(ctx, fd, start, end);
            co_return;
        }

        ctx.set_status(HttpStatus::OK);
        ctx.set_header("Content-Length", std::to_string(file_size));
        ctx.set_content_type(mime_type);

        if (ctx.method() == HttpMethod::HEAD) {
            if (fd.get() >= 0) co_await fd.close();
            co_return;
        }

        if (entry != nullptr) {
            ctx.body(entry->content);
            co_return;
        }

        co_await stream_descriptor(ctx, fd, 0, file_size);
        co_return;
    };
}
} // namespace

Handler static_files(std::string root_dir) {
    StaticFilesConfig config;
    config.root_dir = std::move(root_dir);
    return static_files(std::move(config));
}

Handler static_files(StaticFilesConfig config) {
    std::shared_ptr<FileCache> cache;
    if (config.enable_cache) {
        cache = std::make_shared<FileCache>(config.cache_config);
    }
    return static_files(std::move(config), std::move(cache));
}

Handler static_files(StaticFilesConfig config, FileCache &cache) {
    return make_static_files_handler(std::move(config), nullptr, &cache);
}

Handler static_files(StaticFilesConfig config, std::shared_ptr<FileCache> cache) {
    FileCache *observer = cache.get();
    return make_static_files_handler(std::move(config), std::move(cache), observer);
}

} // namespace kale
