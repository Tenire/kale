#include "doctest/doctest.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "coke/http/http_client.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/http_server.h"
#include "kale/http/file_cache.h"
#include "kale/http/static_files.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix = "kale_static_test_") {
        static std::atomic<uint64_t> counter{0};
        uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
        path_ = std::filesystem::temp_directory_path() /
                (std::string(prefix) + std::to_string(::getpid()) + "_" + std::to_string(id));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;

    const std::filesystem::path &path() const noexcept { return path_; }
    std::string str() const { return path_.string(); }

    std::string file_path(std::string_view rel) const {
        return (path_ / rel).string();
    }

    std::string write_file(std::string_view rel, std::string_view content) const {
        auto full = path_ / rel;
        if (full.has_parent_path()) {
            std::filesystem::create_directories(full.parent_path());
        }
        std::ofstream ofs(full, std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        return full.string();
    }

    bool create_symlink(std::string_view target, std::string_view link_name) const {
        std::error_code ec;
        std::filesystem::create_symlink(path_ / target, path_ / link_name, ec);
        return !ec;
    }

    bool create_directory_symlink(std::string_view target, std::string_view link_name) const {
        std::error_code ec;
        std::filesystem::create_directory_symlink(path_ / target, path_ / link_name, ec);
        return !ec;
    }

private:
    std::filesystem::path path_;
};

struct HttpResponse {
    int state{-1};
    int status{0};
    std::string body;
    std::map<std::string, std::string, std::less<>> headers;

    std::string header(std::string_view name) const {
        auto it = headers.find(name);
        return it != headers.end() ? it->second : "";
    }
};

coke::Task<HttpResponse> http_request(
    std::string url,
    std::string method = "GET",
    std::map<std::string, std::string> req_headers = {}
) {
    auto *task = WFTaskFactory::create_http_task(url, 0, 0, nullptr);
    task->get_req()->set_method(method);
    task->get_req()->add_header_pair("Connection", "close");
    for (const auto &[k, v] : req_headers) {
        task->get_req()->add_header_pair(k, v);
    }
    auto result = co_await coke::HttpAwaiter(task);
    HttpResponse res;
    res.state = result.state;
    if (result.state != 0) co_return res;

    if (const char *status_str = result.resp.get_status_code()) {
        res.status = std::atoi(status_str);
    }
    if (result.resp.is_chunked()) {
        res.body = protocol::HttpUtil::decode_chunked_body(&result.resp);
    } else {
        const void *body_ptr = nullptr;
        size_t body_len = 0;
        result.resp.get_parsed_body(&body_ptr, &body_len);
        if (body_ptr && body_len > 0) {
            res.body.assign(static_cast<const char *>(body_ptr), body_len);
        }
    }
    protocol::HttpHeaderCursor cursor(&result.resp);
    std::string name, val;
    while (cursor.next(name, val)) {
        res.headers[name] = val;
    }
    co_return res;
}

class TestStaticServer {
public:
    ~TestStaticServer() {
        if (server_.is_running()) {
            server_.stop();
        }
    }

    void start() {
        REQUIRE_EQ(server_.start("127.0.0.1", 0), 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(server_.listen_port());
    }

    HttpResponse get(std::string path, std::map<std::string, std::string> hdrs = {}) {
        return coke::sync_wait(http_request(base_url_ + path, "GET", std::move(hdrs)));
    }

    HttpResponse head(std::string path, std::map<std::string, std::string> hdrs = {}) {
        return coke::sync_wait(http_request(base_url_ + path, "HEAD", std::move(hdrs)));
    }

    HttpResponse post(std::string path, std::map<std::string, std::string> hdrs = {}) {
        return coke::sync_wait(http_request(base_url_ + path, "POST", std::move(hdrs)));
    }

    kale::HttpServer &server() { return server_; }
    const std::string &base_url() const { return base_url_; }

private:
    kale::HttpServer server_;
    std::string base_url_;
};

} // namespace

TEST_SUITE("static_cache") {

TEST_CASE("StaticFiles: Basic GET, binary integrity, MIME types, and HEAD semantics") {
    ScopedTempDir tmp;
    tmp.write_file("hello.txt", "Hello Kale Static Server!");
    tmp.write_file("style.css", "body { margin: 0; }");
    tmp.write_file("script.js", "console.log('test');");

    // Binary payload with non-printable and NUL bytes
    std::string binary_data;
    binary_data.resize(4096);
    for (size_t i = 0; i < binary_data.size(); ++i) {
        binary_data[i] = static_cast<char>(i & 0xFF);
    }
    tmp.write_file("sample.bin", binary_data);

    TestStaticServer srv;
    srv.server().GET("/static/*filepath", kale::static_files(tmp.str()));
    srv.server().HEAD("/static/*filepath", kale::static_files(tmp.str()));
    srv.start();

    SUBCASE("Plain text file serving") {
        auto res = srv.get("/static/hello.txt");
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "Hello Kale Static Server!");
        CHECK_EQ(res.header("Content-Type"), "text/plain; charset=utf-8");
        CHECK_EQ(res.header("Content-Length"), std::to_string(std::strlen("Hello Kale Static Server!")));
        CHECK_EQ(res.header("Accept-Ranges"), "bytes");
        CHECK_FALSE(res.header("ETag").empty());
        CHECK_FALSE(res.header("Last-Modified").empty());
    }

    SUBCASE("CSS and JS MIME types") {
        auto res_css = srv.get("/static/style.css");
        CHECK_EQ(res_css.status, 200);
        CHECK_EQ(res_css.header("Content-Type"), "text/css; charset=utf-8");

        auto res_js = srv.get("/static/script.js");
        CHECK_EQ(res_js.status, 200);
        CHECK_EQ(res_js.header("Content-Type"), "application/javascript; charset=utf-8");
    }

    SUBCASE("Binary data integrity") {
        auto res_bin = srv.get("/static/sample.bin");
        CHECK_EQ(res_bin.status, 200);
        CHECK_EQ(res_bin.body.size(), binary_data.size());
        CHECK_EQ(res_bin.body, binary_data);
    }

    SUBCASE("HEAD method sets headers but transmits zero body") {
        auto res_head = srv.head("/static/hello.txt");
        CHECK_EQ(res_head.status, 200);
        CHECK_EQ(res_head.body, "");
        CHECK_EQ(res_head.header("Content-Length"), std::to_string(std::strlen("Hello Kale Static Server!")));
        CHECK_EQ(res_head.header("Content-Type"), "text/plain; charset=utf-8");
        CHECK_FALSE(res_head.header("ETag").empty());
        CHECK_FALSE(res_head.header("Last-Modified").empty());
    }

    SUBCASE("Method Not Allowed on non-GET/HEAD") {
        auto res_post = srv.post("/static/hello.txt");
        CHECK_EQ(res_post.status, 405);
        CHECK_EQ(res_post.header("Allow"), "GET, HEAD");
    }

    SUBCASE("Non-existent file returns 404") {
        auto res_404 = srv.get("/static/non_existent.txt");
        CHECK_EQ(res_404.status, 404);
    }
}

TEST_CASE("StaticFiles: RFC 7233 Byte Range requests and If-Range validation") {
    ScopedTempDir tmp;
    std::string alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    tmp.write_file("alphabet.txt", alphabet);

    TestStaticServer srv;
    srv.server().GET("/static/*filepath", kale::static_files(tmp.str()));
    srv.start();

    auto full_res = srv.get("/static/alphabet.txt");
    REQUIRE_EQ(full_res.status, 200);
    std::string etag = full_res.header("ETag");
    std::string last_mod = full_res.header("Last-Modified");
    REQUIRE_FALSE(etag.empty());

    SUBCASE("Standard closed range: bytes=0-9") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=0-9"}});
        CHECK_EQ(res.status, 206);
        CHECK_EQ(res.body, "0123456789");
        CHECK_EQ(res.header("Content-Range"), "bytes 0-9/" + std::to_string(alphabet.size()));
        CHECK_EQ(res.header("Content-Length"), "10");
    }

    SUBCASE("Suffix range: bytes=-10") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=-10"}});
        CHECK_EQ(res.status, 206);
        CHECK_EQ(res.body, alphabet.substr(alphabet.size() - 10));
        CHECK_EQ(res.header("Content-Length"), "10");
    }

    SUBCASE("Open-ended range: bytes=10-") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=10-"}});
        CHECK_EQ(res.status, 206);
        CHECK_EQ(res.body, alphabet.substr(10));
        CHECK_EQ(res.header("Content-Length"), std::to_string(alphabet.size() - 10));
    }

    SUBCASE("Unsatisfiable range: bytes=100-200") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=100-200"}});
        CHECK_EQ(res.status, 416);
        CHECK_EQ(res.header("Content-Range"), "bytes */" + std::to_string(alphabet.size()));
        CHECK_EQ(res.header("Content-Length"), "0");
    }

    SUBCASE("If-Range with matching strong ETag satisfies range") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=0-4"}, {"If-Range", etag}});
        CHECK_EQ(res.status, 206);
        CHECK_EQ(res.body, "01234");
    }

    SUBCASE("If-Range with mismatched ETag ignores Range and returns 200 OK full body") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=0-4"}, {"If-Range", "\"mismatched-etag\""}});
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, alphabet);
    }

    SUBCASE("If-Range with weak ETag ignores Range per RFC 7233") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=0-4"}, {"If-Range", "W/" + etag}});
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, alphabet);
    }

    SUBCASE("If-Range with matching HTTP-date satisfies range") {
        auto res = srv.get("/static/alphabet.txt", {{"Range", "bytes=0-4"}, {"If-Range", last_mod}});
        CHECK_EQ(res.status, 206);
        CHECK_EQ(res.body, "01234");
    }
}

TEST_CASE("StaticFiles: RFC 7232 Conditional validators and precedence rules") {
    ScopedTempDir tmp;
    tmp.write_file("resource.txt", "Resource representation data");

    TestStaticServer srv;
    srv.server().GET("/static/*filepath", kale::static_files(tmp.str()));
    srv.start();

    auto init_res = srv.get("/static/resource.txt");
    REQUIRE_EQ(init_res.status, 200);
    std::string etag = init_res.header("ETag");
    std::string last_mod = init_res.header("Last-Modified");

    SUBCASE("If-None-Match matching returns 304 Not Modified") {
        auto res = srv.get("/static/resource.txt", {{"If-None-Match", etag}});
        CHECK_EQ(res.status, 304);
        CHECK_EQ(res.body, "");
        CHECK_EQ(res.header("ETag"), etag);
        CHECK_EQ(res.header("Last-Modified"), last_mod);
    }

    SUBCASE("If-None-Match weak tag matching returns 304") {
        auto res = srv.get("/static/resource.txt", {{"If-None-Match", "W/" + etag}});
        CHECK_EQ(res.status, 304);
    }

    SUBCASE("If-None-Match wildcard * returns 304") {
        auto res = srv.get("/static/resource.txt", {{"If-None-Match", "*"}});
        CHECK_EQ(res.status, 304);
    }

    SUBCASE("If-None-Match mismatch returns 200 OK") {
        auto res = srv.get("/static/resource.txt", {{"If-None-Match", "\"different-tag\""}});
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "Resource representation data");
    }

    SUBCASE("If-None-Match suppresses If-Modified-Since per RFC 7232 §3.3") {
        // If-None-Match differs (demands 200), even if If-Modified-Since is in the future
        auto res = srv.get("/static/resource.txt", {
            {"If-None-Match", "\"different-tag\""},
            {"If-Modified-Since", "Sun, 01 Jan 2035 00:00:00 GMT"}
        });
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "Resource representation data");
    }

    SUBCASE("If-Modified-Since evaluated when If-None-Match absent") {
        auto res_future = srv.get("/static/resource.txt", {
            {"If-Modified-Since", "Sun, 01 Jan 2035 00:00:00 GMT"}
        });
        CHECK_EQ(res_future.status, 304);

        auto res_past = srv.get("/static/resource.txt", {
            {"If-Modified-Since", "Sun, 01 Jan 2000 00:00:00 GMT"}
        });
        CHECK_EQ(res_past.status, 200);
    }

    SUBCASE("If-Match mismatch returns 412 Precondition Failed") {
        auto res = srv.get("/static/resource.txt", {{"If-Match", "\"wrong-tag\""}});
        CHECK_EQ(res.status, 412);
    }

    SUBCASE("If-Match matching succeeds with 200 OK") {
        auto res = srv.get("/static/resource.txt", {{"If-Match", etag}});
        CHECK_EQ(res.status, 200);
    }
}

TEST_CASE("StaticFiles: Directory index resolution and 301 trailing-slash redirect") {
    ScopedTempDir tmp;
    tmp.write_file("index.html", "<h1>Root Index</h1>");
    tmp.write_file("docs/index.html", "<h1>Docs Index</h1>");
    tmp.write_file("docs/guide.txt", "Guide details");

    TestStaticServer srv;
    srv.server().GET("/static/*filepath", kale::static_files(tmp.str()));
    srv.start();

    SUBCASE("Directory request with trailing slash serves index.html") {
        auto res = srv.get("/static/docs/");
        CHECK_EQ(res.status, 200);
        CHECK_EQ(res.body, "<h1>Docs Index</h1>");
        CHECK_EQ(res.header("Content-Type"), "text/html; charset=utf-8");
    }

    SUBCASE("Directory request without trailing slash redirects 301 to trailing slash") {
        auto res = srv.get("/static/docs");
        CHECK_EQ(res.status, 301);
        CHECK_EQ(res.header("Location"), "/static/docs/");
    }

    SUBCASE("Redirect preserves query parameters safely") {
        auto res = srv.get("/static/docs?view=full&lang=en");
        CHECK_EQ(res.status, 301);
        CHECK_EQ(res.header("Location"), "/static/docs/?view=full&lang=en");
    }
}

TEST_CASE("StaticFiles: Traversal, NUL, encoded separators, and symlink security") {
    ScopedTempDir tmp;
    tmp.write_file("public.txt", "public content");
    tmp.write_file("sub/file.txt", "nested content");

    kale::StaticFilesConfig sym_cfg;
    sym_cfg.root_dir = tmp.str();
    sym_cfg.url_prefix = "/static_sym";
    sym_cfg.allow_symlinks = true;

    TestStaticServer srv;
    srv.server().GET("/static/*filepath", kale::static_files(tmp.str()));
    srv.server().GET("/static_sym/*filepath", kale::static_files(sym_cfg));
    srv.start();

    SUBCASE("Direct .. traversal is forbidden (403)") {
        auto res = srv.get("/static/../secret.txt");
        CHECK_EQ(res.status, 403);
    }

    SUBCASE("Nested .. traversal is forbidden (403)") {
        auto res = srv.get("/static/sub/../../secret.txt");
        CHECK_EQ(res.status, 403);
    }

    SUBCASE("Encoded slash %2f is rejected (400)") {
        auto res = srv.get("/static/sub%2ffile.txt");
        CHECK_EQ(res.status, 400);
    }

    SUBCASE("NUL character is rejected (400)") {
        auto res = srv.get("/static/public.txt%00");
        CHECK_EQ(res.status, 400);
    }

    SUBCASE("Symlinks under O_NOFOLLOW policy are rejected (403)") {
        if (tmp.create_symlink("public.txt", "symlink_file.txt")) {
            auto res = srv.get("/static/symlink_file.txt");
            CHECK_EQ(res.status, 403);
        }
    }

    SUBCASE("Internal file symlink: default rejects, allow_symlinks=true serves exact expected bytes") {
        REQUIRE(tmp.create_symlink("public.txt", "symlink_file_allow.txt"));
        auto res_default = srv.get("/static/symlink_file_allow.txt");
        CHECK_EQ(res_default.status, 403);

        auto res_allow = srv.get("/static_sym/symlink_file_allow.txt");
        CHECK_EQ(res_allow.status, 200);
        CHECK_EQ(res_allow.body, "public content");
    }

    SUBCASE("Internal directory symlink: default rejects, allow_symlinks=true serves exact expected bytes") {
        REQUIRE(tmp.create_directory_symlink("sub", "symlink_dir"));
        auto res_default = srv.get("/static/symlink_dir/file.txt");
        CHECK_EQ(res_default.status, 403);

        auto res_allow = srv.get("/static_sym/symlink_dir/file.txt");
        CHECK_EQ(res_allow.status, 200);
        CHECK_EQ(res_allow.body, "nested content");
    }
}

TEST_CASE("FileCache: Capacity bounds, payload budget eviction, and mtime invalidation") {
    ScopedTempDir tmp;
    tmp.write_file("file1.txt", "1111111111"); // 10 bytes
    tmp.write_file("file2.txt", "2222222222"); // 10 bytes
    tmp.write_file("file3.txt", "3333333333"); // 10 bytes

    kale::FileCacheConfig cfg;
    cfg.capacity = 2; // Maximum 2 entries
    cfg.max_cache_bytes = 25; // Maximum 25 bytes
    cfg.check_mtime = true;

    auto cache = std::make_shared<kale::FileCache>(cfg);

    kale::StaticFilesConfig files_cfg;
    files_cfg.root_dir = tmp.str();
    files_cfg.enable_cache = true;
    files_cfg.cache_config = cfg;

    TestStaticServer srv;
    srv.server().GET("/static/*filepath", kale::static_files(files_cfg, cache));
    srv.start();

    SUBCASE("LRU capacity eviction bounds resident entry count") {
        srv.get("/static/file1.txt");
        srv.get("/static/file2.txt");
        CHECK_EQ(cache->size(), 2);
        CHECK_EQ(cache->resident_bytes(), 20);

        // Fetching third file evicts oldest (file1.txt)
        srv.get("/static/file3.txt");
        CHECK_EQ(cache->size(), 2);
        CHECK_EQ(cache->resident_bytes(), 20);
        CHECK_EQ(cache->get_resident(tmp.file_path("file1.txt")), nullptr);
        CHECK_NE(cache->get_resident(tmp.file_path("file2.txt")), nullptr);
        CHECK_NE(cache->get_resident(tmp.file_path("file3.txt")), nullptr);
    }

    SUBCASE("Mtime change triggers stale invalidation and content reload") {
        auto res1 = srv.get("/static/file1.txt");
        CHECK_EQ(res1.body, "1111111111");

        // Delay to ensure timestamp tick
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        tmp.write_file("file1.txt", "UPDATED_CONTENT");

        auto res2 = srv.get("/static/file1.txt");
        CHECK_EQ(res2.status, 200);
        CHECK_EQ(res2.body, "UPDATED_CONTENT");
    }

    SUBCASE("Cache clear empties resident table without invalidating active references") {
        auto res = srv.get("/static/file2.txt");
        CHECK_EQ(res.status, 200);
        auto entry_ref = cache->get_resident(tmp.file_path("file2.txt"));
        REQUIRE_NE(entry_ref, nullptr);

        cache->clear();
        CHECK_EQ(cache->size(), 0);
        CHECK_EQ(cache->resident_bytes(), 0);
        // Active entry pin survives cache clear
        CHECK_EQ(entry_ref->content, "2222222222");
    }
}

TEST_CASE("StaticFiles: Shared FileCache isolates distinct roots with identical relative paths") {
    ScopedTempDir root_a;
    ScopedTempDir root_b;
    root_a.write_file("file.txt", "PAYLOAD_ALPHA_12345");
    root_b.write_file("file.txt", "PAYLOAD_BRAVO_67890");

    kale::FileCacheConfig cache_cfg;
    cache_cfg.check_mtime = false;
    kale::FileCache cache(cache_cfg);

    kale::StaticFilesConfig cfg_a;
    cfg_a.root_dir = root_a.str();
    cfg_a.url_prefix = "/static_a";
    cfg_a.enable_cache = true;
    cfg_a.cache_config = cache_cfg;

    kale::StaticFilesConfig cfg_b;
    cfg_b.root_dir = root_b.str();
    cfg_b.url_prefix = "/static_b";
    cfg_b.enable_cache = true;
    cfg_b.cache_config = cache_cfg;

    TestStaticServer srv;
    srv.server().GET("/static_a/*filepath", kale::static_files(cfg_a, cache));
    srv.server().GET("/static_b/*filepath", kale::static_files(cfg_b, cache));
    srv.start();

    auto res_a1 = srv.get("/static_a/file.txt");
    CHECK_EQ(res_a1.status, 200);
    CHECK_EQ(res_a1.body, "PAYLOAD_ALPHA_12345");

    auto res_b = srv.get("/static_b/file.txt");
    CHECK_EQ(res_b.status, 200);
    CHECK_EQ(res_b.body, "PAYLOAD_BRAVO_67890");

    auto res_a2 = srv.get("/static_a/file.txt");
    CHECK_EQ(res_a2.status, 200);
    CHECK_EQ(res_a2.body, "PAYLOAD_ALPHA_12345");
}
} // TEST_SUITE("static_cache")
