#include "doctest/doctest.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/task.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/router.h"
#include "kale/security/session.h"
#include "kale/http/sse.h"
#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"
#include "workflow/WFTaskFactory.h"

namespace {

struct SessionTestResponse {
    int state{0};
    int error{0};
    int status{0};
    std::string status_str;
    std::string body;
    std::map<std::string, std::string, kale::CaseInsensitiveLess> headers;
    std::vector<std::string> set_cookies;

    bool ok() const noexcept { return state == 0; }

    std::string header(std::string_view key) const {
        auto it = headers.find(key);
        return (it != headers.end()) ? it->second : std::string{};
    }

    static SessionTestResponse from_coke(coke::HttpResult &res) {
        SessionTestResponse out;
        out.state = res.state;
        out.error = res.error;
        if (res.state == 0) {
            const char *sc = res.resp.get_status_code();
            if (sc) {
                out.status_str = sc;
                std::from_chars(out.status_str.data(), out.status_str.data() + out.status_str.size(), out.status);
            }
            if (res.resp.is_chunked()) {
                out.body = protocol::HttpUtil::decode_chunked_body(&res.resp);
            } else {
                const void *body_ptr = nullptr;
                size_t body_len = 0;
                if (res.resp.get_parsed_body(&body_ptr, &body_len) && body_ptr && body_len > 0) {
                    out.body.assign(static_cast<const char *>(body_ptr), body_len);
                }
            }
            protocol::HttpHeaderCursor cursor(&res.resp);
            std::string name, val;
            while (cursor.next(name, val)) {
                if (kale::CaseInsensitiveLess{}(name, "Set-Cookie") == false &&
                    kale::CaseInsensitiveLess{}("Set-Cookie", name) == false) {
                    out.set_cookies.push_back(val);
                }
                out.headers[name] = val;
            }
        }
        return out;
    }
};

using HeaderPairList = std::vector<std::pair<std::string, std::string>>;

inline coke::Task<SessionTestResponse> do_session_http_request(
    std::string url,
    std::string method = "GET",
    HeaderPairList headers = {},
    std::string body = "") {
    auto *task = WFTaskFactory::create_http_task(url, 0, 1, nullptr);
    auto *req = task->get_req();
    req->set_method(method);
    for (const auto &pair : headers) {
        req->add_header_pair(pair.first, pair.second);
    }
    if (!body.empty()) {
        req->append_output_body(body.data(), body.size());
    }
    auto res = co_await coke::HttpAwaiter(task);
    co_return SessionTestResponse::from_coke(res);
}

class SessionServerFixture {
public:
    explicit SessionServerFixture(kale::HttpServerParams params = {})
        : server_(params) {
        int ret = server_.start("127.0.0.1", 0);
        REQUIRE_EQ(ret, 0);
        REQUIRE(server_.is_running());
        port_ = static_cast<unsigned short>(server_.listen_port());
        REQUIRE_GT(port_, 0);
        base_url_ = "http://127.0.0.1:" + std::to_string(port_);
    }

    ~SessionServerFixture() {
        if (server_.is_running()) {
            server_.stop();
        }
    }

    SessionServerFixture(const SessionServerFixture &) = delete;
    SessionServerFixture &operator=(const SessionServerFixture &) = delete;

    kale::HttpServer *operator->() noexcept { return &server_; }
    kale::HttpServer &server() noexcept { return server_; }
    unsigned short port() const noexcept { return port_; }
    const std::string &base_url() const noexcept { return base_url_; }

    std::string url(std::string_view path) const {
        if (path.empty() || path.front() != '/') {
            return base_url_ + "/" + std::string(path);
        }
        return base_url_ + std::string(path);
    }

    SessionTestResponse request(
        std::string_view path,
        std::string method = "GET",
        HeaderPairList headers = {},
        std::string body = "") const {
        return coke::sync_wait(
            do_session_http_request(url(path), std::move(method), std::move(headers), std::move(body))
        );
    }

    SessionTestResponse get(std::string_view path, HeaderPairList headers = {}) const {
        return request(path, "GET", std::move(headers), "");
    }

    SessionTestResponse post(std::string_view path, std::string body = "", HeaderPairList headers = {}) const {
        return request(path, "POST", std::move(headers), std::move(body));
    }

private:
    kale::HttpServer server_;
    unsigned short port_{0};
    std::string base_url_;
};

// Tracking store decorator
class CountingStore : public kale::SessionStore {
public:
    explicit CountingStore(std::shared_ptr<kale::SessionStore> inner = std::make_shared<kale::MemorySessionStore>())
        : inner_(std::move(inner)) {}

    std::atomic<int> load_count{0};
    std::atomic<int> insert_count{0};
    std::atomic<int> update_count{0};
    std::atomic<int> replace_count{0};
    std::atomic<int> destroy_count{0};

    coke::Task<std::optional<kale::SessionRecord>> load(std::string id) override {
        load_count++;
        return inner_->load(std::move(id));
    }
    coke::Task<void> insert(std::string id, kale::SessionRecord record) override {
        insert_count++;
        return inner_->insert(std::move(id), std::move(record));
    }
    coke::Task<kale::SessionRecord> update(
        std::string id,
        std::uint64_t expected_version,
        kale::SessionPatch patch,
        std::chrono::system_clock::time_point expires_at) override {
        update_count++;
        return inner_->update(std::move(id), expected_version, std::move(patch), expires_at);
    }
    coke::Task<void> replace(
        std::string old_id,
        std::uint64_t expected_version,
        std::string new_id,
        kale::SessionRecord replacement) override {
        replace_count++;
        return inner_->replace(std::move(old_id), expected_version, std::move(new_id), std::move(replacement));
    }
    coke::Task<void> destroy(std::string id) override {
        destroy_count++;
        return inner_->destroy(std::move(id));
    }

private:
    std::shared_ptr<kale::SessionStore> inner_;
};

// Failing store for failure-propagation testing
class FailingStore : public kale::SessionStore {
public:
    bool fail_load{false};
    bool fail_insert{false};
    bool fail_update{false};
    bool fail_replace{false};
    bool fail_destroy{false};

    coke::Task<std::optional<kale::SessionRecord>> load(std::string) override {
        if (fail_load) throw kale::SessionStoreError("Simulated load failure");
        co_return std::nullopt;
    }
    coke::Task<void> insert(std::string, kale::SessionRecord) override {
        if (fail_insert) throw kale::SessionStoreError("Simulated insert failure");
        co_return;
    }
    coke::Task<kale::SessionRecord> update(
        std::string, std::uint64_t, kale::SessionPatch, std::chrono::system_clock::time_point) override {
        if (fail_update) throw kale::SessionStoreError("Simulated update failure");
        co_return kale::SessionRecord{};
    }
    coke::Task<void> replace(
        std::string, std::uint64_t, std::string, kale::SessionRecord) override {
        if (fail_replace) throw kale::SessionStoreError("Simulated replace failure");
        co_return;
    }
    coke::Task<void> destroy(std::string) override {
        if (fail_destroy) throw kale::SessionStoreError("Simulated destroy failure");
        co_return;
    }
};

// Custom SessionStore subclass
class CustomUserStore : public kale::SessionStore {
public:
    std::map<std::string, kale::SessionRecord> map_data;
    std::mutex mtx;

    coke::Task<std::optional<kale::SessionRecord>> load(std::string id) override {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map_data.find(id);
        if (it != map_data.end()) {
            if (it->second.expires_at <= std::chrono::system_clock::now()) {
                map_data.erase(it);
                co_return std::nullopt;
            }
            co_return it->second;
        }
        co_return std::nullopt;
    }
    coke::Task<void> insert(std::string id, kale::SessionRecord record) override {
        std::lock_guard<std::mutex> lk(mtx);
        if (map_data.find(id) != map_data.end()) {
            throw kale::SessionConflict("CustomUserStore: id conflict");
        }
        map_data[std::move(id)] = std::move(record);
        co_return;
    }
    coke::Task<kale::SessionRecord> update(
        std::string id,
        std::uint64_t expected_version,
        kale::SessionPatch patch,
        std::chrono::system_clock::time_point expires_at) override {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map_data.find(id);
        if (it == map_data.end()) throw kale::SessionMissing("CustomUserStore: missing");
        if (it->second.version != expected_version) throw kale::SessionConflict("CustomUserStore: version conflict");
        for (const auto &e : patch.erase) it->second.values.erase(e);
        for (auto &s : patch.set) it->second.values[s.first] = s.second;
        it->second.version = expected_version + 1;
        it->second.expires_at = expires_at;
        co_return it->second;
    }
    coke::Task<void> replace(
        std::string old_id,
        std::uint64_t expected_version,
        std::string new_id,
        kale::SessionRecord replacement) override {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = map_data.find(old_id);
        if (it == map_data.end()) throw kale::SessionMissing("CustomUserStore: old missing");
        if (it->second.version != expected_version) throw kale::SessionConflict("CustomUserStore: version conflict");
        map_data.erase(it);
        map_data[std::move(new_id)] = std::move(replacement);
        co_return;
    }
    coke::Task<void> destroy(std::string id) override {
        std::lock_guard<std::mutex> lk(mtx);
        map_data.erase(id);
        co_return;
    }
};

} // namespace

TEST_SUITE("core") {

// ----------------------------------------------------------------------------
// 1. Middleware Installation & Missing-Middleware Error
// ----------------------------------------------------------------------------
TEST_CASE("Session: middleware installation and missing-middleware error") {
    SessionServerFixture fixture_no_mw;
    fixture_no_mw->GET("/test-missing", [](kale::Context &ctx) {
        CHECK_THROWS_AS(kale::get_session(ctx), std::logic_error);
        ctx.string("ok_missing");
    });

    auto resp_missing = fixture_no_mw.get("/test-missing");
    CHECK(resp_missing.ok());
    CHECK_EQ(resp_missing.body, "ok_missing");

    kale::MemorySessionStore store;
    SessionServerFixture fixture_with_mw;
    fixture_with_mw->use(kale::session(store));
    fixture_with_mw->GET("/test-present", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        ctx.string("ok_present");
    });

    auto resp_present = fixture_with_mw.get("/test-present");
    CHECK(resp_present.ok());
    CHECK_EQ(resp_present.body, "ok_present");
}

// ----------------------------------------------------------------------------
// 2. Config Validation
// ----------------------------------------------------------------------------
TEST_CASE("Session: config validation") {
    kale::MemorySessionStore store;

    // ttl <= 0
    {
        kale::SessionOptions opts;
        opts.ttl = std::chrono::seconds(0);
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);
        opts.ttl = std::chrono::seconds(-10);
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);
    }

    // SameSite::None without secure
    {
        kale::SessionOptions opts;
        opts.same_site = kale::SessionSameSite::None;
        opts.secure = false;
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.secure = true;
        CHECK_NOTHROW(kale::session(store, opts));
    }

    // Invalid cookie_name
    {
        kale::SessionOptions opts;
        opts.cookie_name = "";
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.cookie_name = "bad=name";
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.cookie_name = "bad;name";
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.cookie_name = "bad name";
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.cookie_name = "bad\tname";
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);
    }

    // Invalid cookie_path
    {
        kale::SessionOptions opts;
        opts.cookie_path = "";
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.cookie_path = "relative/path";
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.cookie_path = "/valid/path";
        CHECK_NOTHROW(kale::session(store, opts));
    }
}

TEST_CASE("Session: TTL cookie Max-Age overflow and invalid cookie options must reject before store mutation") {
    kale::MemorySessionStore store;

    SUBCASE("TTL exceeding cookie Max-Age integer capacity must be rejected by middleware") {
        kale::SessionOptions opts;
        // 4294967296 seconds = 2^32 seconds. Positive chrono::seconds, but overflows 32-bit int Max-Age to 0
        opts.ttl = std::chrono::seconds(4294967296LL);
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.ttl = std::chrono::seconds(static_cast<long long>(std::numeric_limits<int>::max()) + 1);
        CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);

        opts.ttl = std::chrono::seconds(std::numeric_limits<int>::max());
        CHECK_NOTHROW(kale::session(store, opts));
    }

    SUBCASE("Invalid cookie_name containing RFC token separators must be rejected at config time") {
        for (const char *bad_name : {"bad/name", "bad@name", "bad,name", "bad:name", "bad?name"}) {
            kale::SessionOptions opts;
            opts.cookie_name = bad_name;
            CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);
        }
    }

    SUBCASE("Invalid cookie_path containing invalid characters must be rejected at config time") {
        for (const char *bad_path : {"/bad;path", "/bad\r\npath"}) {
            kale::SessionOptions opts;
            opts.cookie_path = bad_path;
            CHECK_THROWS_AS(kale::session(store, opts), std::invalid_argument);
        }
    }

}

// ----------------------------------------------------------------------------
// 3. Anonymous Read-Only (0 Store Calls, 0 Set-Cookie)
// ----------------------------------------------------------------------------
TEST_CASE("Session: anonymous read-only produces zero store calls and zero Set-Cookie") {
    auto counting_store = std::make_shared<CountingStore>();
    SessionServerFixture fixture;
    fixture->use(kale::session(*counting_store));

    fixture->GET("/inspect-anon", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        CHECK_FALSE(sess.contains("user"));
        CHECK_FALSE(sess.get("user").has_value());
        CHECK_EQ(sess.version(), 0);
        CHECK(sess.id().empty());
        ctx.string("anon_ok");
    });

    auto resp = fixture.get("/inspect-anon");
    CHECK(resp.ok());
    CHECK_EQ(resp.body, "anon_ok");

    // Zero Set-Cookie headers
    CHECK(resp.set_cookies.empty());

    // Zero store calls
    CHECK_EQ(counting_store->load_count.load(), 0);
    CHECK_EQ(counting_store->insert_count.load(), 0);
    CHECK_EQ(counting_store->update_count.load(), 0);
    CHECK_EQ(counting_store->replace_count.load(), 0);
    CHECK_EQ(counting_store->destroy_count.load(), 0);
}

// ----------------------------------------------------------------------------
// 4. Unknown/Expired Cookie ID Treated as Anonymous
// ----------------------------------------------------------------------------
TEST_CASE("Session: unknown or expired cookie ID treated as anonymous without Set-Cookie") {
    auto counting_store = std::make_shared<CountingStore>();

    // Pre-insert an expired record
    std::string expired_id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    kale::SessionRecord expired_rec;
    expired_rec.values["expired_key"] = "expired_val";
    expired_rec.version = 1;
    expired_rec.expires_at = std::chrono::system_clock::now() - std::chrono::seconds(100);
    coke::sync_wait(counting_store->insert(expired_id, expired_rec));

    SessionServerFixture fixture;
    fixture->use(kale::session(*counting_store));

    fixture->GET("/check", [](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        ctx.string("check_done");
    });

    // 1. Unknown cookie ID
    auto resp_unknown = fixture.get("/check", {{"Cookie", "kale_session=nonexistent_id_404"}});
    CHECK(resp_unknown.ok());
    CHECK_EQ(resp_unknown.body, "check_done");
    CHECK(resp_unknown.set_cookies.empty());

    // 2. Expired cookie ID
    auto resp_expired = fixture.get("/check", {{"Cookie", "kale_session=" + expired_id}});
    CHECK(resp_expired.ok());
    CHECK_EQ(resp_expired.body, "check_done");
    CHECK(resp_expired.set_cookies.empty());
}

// ----------------------------------------------------------------------------
// 5. Create and Duplicate Create Error
// ----------------------------------------------------------------------------
TEST_CASE("Session: create and duplicate create error") {
    kale::MemorySessionStore store;
    SessionServerFixture fixture;
    fixture->use(kale::session(store));

    fixture->POST("/login", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        co_await sess.create({{"username", "alice"}, {"role", "editor"}});
        CHECK(sess.exists());
        CHECK_EQ(sess.version(), 1);
        CHECK_EQ(sess.id().size(), 64); // 32 bytes hex
        CHECK_EQ(sess.get("username"), "alice");
        CHECK_EQ(sess.get("role"), "editor");

        // Duplicate create on already active session MUST throw SessionConflict
        bool duplicate_threw = false;
        try {
            co_await sess.create({{"username", "bob"}});
        } catch (const kale::SessionConflict &) {
            duplicate_threw = true;
        }
        CHECK(duplicate_threw);

        ctx.string(sess.id());
    });

    auto resp = fixture.post("/login");
    CHECK(resp.ok());
    std::string created_id = resp.body;
    CHECK_EQ(created_id.size(), 64);

    REQUIRE_FALSE(resp.set_cookies.empty());
    CHECK(resp.set_cookies[0].find("kale_session=" + created_id) != std::string::npos);
    CHECK(resp.set_cookies[0].find("HttpOnly") != std::string::npos);
    CHECK(resp.set_cookies[0].find("SameSite=Lax") != std::string::npos);

    // Verify record in store
    auto rec = coke::sync_wait(store.load(created_id));
    REQUIRE(rec.has_value());
    CHECK_EQ(rec->version, 1);
    CHECK_EQ(rec->values.at("username"), "alice");
    CHECK_EQ(rec->values.at("role"), "editor");
}

// ----------------------------------------------------------------------------
// 6. Login Regenerate (from Anonymous and Existing Session, Old ID Invalidated)
// ----------------------------------------------------------------------------
TEST_CASE("Session: login regenerate from anonymous and existing session with old ID invalidation") {
    kale::MemorySessionStore store;
    SessionServerFixture fixture;
    fixture->use(kale::session(store));

    // Endpoint 1: regenerate from anonymous
    fixture->POST("/login-anon", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        co_await sess.regenerate({{"user", "bob"}});
        CHECK(sess.exists());
        CHECK_EQ(sess.version(), 1);
        CHECK_EQ(sess.get("user"), "bob");
        ctx.string(sess.id());
    });

    // Endpoint 2: regenerate from existing
    fixture->POST("/login-elevate", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK(sess.exists());
        CHECK_EQ(sess.get("user"), "bob");
        std::string old_id(sess.id());

        co_await sess.regenerate({{"user", "bob_admin"}});
        CHECK(sess.exists());
        CHECK_NE(sess.id(), old_id);
        CHECK_EQ(sess.version(), 1);
        CHECK_EQ(sess.get("user"), "bob_admin");
        ctx.string(sess.id());
    });

    // 1. Anon login
    auto resp1 = fixture.post("/login-anon");
    CHECK(resp1.ok());
    std::string new_id1 = resp1.body;
    CHECK_EQ(new_id1.size(), 64);
    REQUIRE_FALSE(resp1.set_cookies.empty());
    CHECK(resp1.set_cookies[0].find("kale_session=" + new_id1) != std::string::npos);

    auto rec1 = coke::sync_wait(store.load(new_id1));
    REQUIRE(rec1.has_value());
    CHECK_EQ(rec1->values.at("user"), "bob");
    CHECK_EQ(rec1->version, 1);

    // 2. Elevate from existing session
    auto resp2 = fixture.post("/login-elevate", "", {{"Cookie", "kale_session=" + new_id1}});
    CHECK(resp2.ok());
    std::string new_id2 = resp2.body;
    CHECK_EQ(new_id2.size(), 64);
    CHECK_NE(new_id2, new_id1);

    // Old ID MUST be completely invalidated in store!
    auto old_check = coke::sync_wait(store.load(new_id1));
    CHECK_FALSE(old_check.has_value());

    // New ID exists in store with version 1
    auto new_check = coke::sync_wait(store.load(new_id2));
    REQUIRE(new_check.has_value());
    CHECK_EQ(new_check->values.at("user"), "bob_admin");
    CHECK_EQ(new_check->version, 1);
}

// ----------------------------------------------------------------------------
// 7. Update, Set, Erase, and Overlapping Patch Key Rejection
// ----------------------------------------------------------------------------
TEST_CASE("Session: update, set, erase, and overlapping patch key rejection") {
    kale::MemorySessionStore store;
    SessionServerFixture fixture;
    fixture->use(kale::session(store));

    fixture->POST("/setup", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        co_await sess.create({{"key1", "val1"}, {"key2", "val2"}});
        ctx.string(sess.id());
    });

    fixture->POST("/modify", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        REQUIRE(sess.exists());

        // Overlapping patch key rejection
        kale::SessionPatch invalid_patch;
        invalid_patch.set["k"] = "v";
        invalid_patch.erase.push_back("k");
        CHECK_THROWS_AS(co_await sess.update(invalid_patch), std::invalid_argument);

        // Valid set
        co_await sess.set("key3", "val3");
        CHECK_EQ(sess.get("key3"), "val3");
        CHECK_EQ(sess.version(), 2);

        // Valid erase
        co_await sess.erase("key1");
        CHECK_FALSE(sess.contains("key1"));
        CHECK_EQ(sess.version(), 3);

        ctx.string("modified");
    });

    fixture->POST("/anon-modify", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        CHECK_THROWS_AS(co_await sess.set("k", "v"), kale::SessionMissing);
        CHECK_THROWS_AS(co_await sess.erase("k"), kale::SessionMissing);
        ctx.string("anon_modified_tested");
    });

    auto setup_resp = fixture.post("/setup");
    CHECK(setup_resp.ok());
    std::string sess_id = setup_resp.body;

    auto mod_resp = fixture.post("/modify", "", {{"Cookie", "kale_session=" + sess_id}});
    CHECK(mod_resp.ok());
    CHECK_EQ(mod_resp.body, "modified");

    // Verify in store
    auto rec = coke::sync_wait(store.load(sess_id));
    REQUIRE(rec.has_value());
    CHECK_EQ(rec->version, 3);
    CHECK_FALSE(rec->values.count("key1"));
    CHECK_EQ(rec->values.at("key2"), "val2");
    CHECK_EQ(rec->values.at("key3"), "val3");

    auto anon_resp = fixture.post("/anon-modify");
    CHECK(anon_resp.ok());
}

// ----------------------------------------------------------------------------
// 8. Renew Refreshes Expiration and Writes Refreshed Set-Cookie
// ----------------------------------------------------------------------------
TEST_CASE("Session: renew refreshes expiration and writes refreshed Set-Cookie") {
    kale::MemorySessionStore store;
    SessionServerFixture fixture;
    fixture->use(kale::session(store));

    fixture->POST("/create", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        co_await sess.create({{"user", "tester"}});
        ctx.string(sess.id());
    });

    fixture->POST("/renew", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        REQUIRE(sess.exists());
        auto v_before = sess.version();
        co_await sess.renew();
        CHECK_EQ(sess.version(), v_before + 1);
        ctx.string(sess.id());
    });

    fixture->POST("/renew-anon", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        CHECK_THROWS_AS(co_await sess.renew(), kale::SessionMissing);
        ctx.string("anon_renew_caught");
    });

    auto cr = fixture.post("/create");
    CHECK(cr.ok());
    std::string sess_id = cr.body;

    auto rec_before = coke::sync_wait(store.load(sess_id));
    REQUIRE(rec_before.has_value());

    auto ren = fixture.post("/renew", "", {{"Cookie", "kale_session=" + sess_id}});
    CHECK(ren.ok());
    REQUIRE_FALSE(ren.set_cookies.empty());
    CHECK(ren.set_cookies[0].find("kale_session=" + sess_id) != std::string::npos);

    auto rec_after = coke::sync_wait(store.load(sess_id));
    REQUIRE(rec_after.has_value());
    CHECK_EQ(rec_after->version, rec_before->version + 1);
    CHECK_GE(rec_after->expires_at, rec_before->expires_at);

    auto anon_ren = fixture.post("/renew-anon");
    CHECK(anon_ren.ok());
}

// ----------------------------------------------------------------------------
// 9. Version Conflict (SessionConflict on Concurrent Update)
// ----------------------------------------------------------------------------
TEST_CASE("Session: version conflict handling on concurrent requests") {
    kale::MemorySessionStore store;
    std::string sess_id = "concurrent_session_id_1234567890abcdef12345678901234567890abcdef";
    kale::SessionRecord initial_rec;
    initial_rec.values["counter"] = "0";
    initial_rec.version = 1;
    initial_rec.expires_at = std::chrono::system_clock::now() + std::chrono::seconds(1800);
    coke::sync_wait(store.insert(sess_id, initial_rec));

    // Two simulated concurrent requests load version 1
    auto rec_a = coke::sync_wait(store.load(sess_id));
    auto rec_b = coke::sync_wait(store.load(sess_id));
    REQUIRE(rec_a.has_value());
    REQUIRE(rec_b.has_value());
    CHECK_EQ(rec_a->version, 1);
    CHECK_EQ(rec_b->version, 1);

    // Request A updates version 1 -> 2
    kale::SessionPatch patch_a;
    patch_a.set["counter"] = "1";
    auto updated_a = coke::sync_wait(
        store.update(sess_id, 1, patch_a, std::chrono::system_clock::now() + std::chrono::seconds(1800))
    );
    CHECK_EQ(updated_a.version, 2);
    CHECK_EQ(updated_a.values.at("counter"), "1");

    // Request B attempts to update with stale expected version 1 -> throws SessionConflict!
    kale::SessionPatch patch_b;
    patch_b.set["counter"] = "2";
    bool conflict_caught = false;
    coke::sync_wait([&]() -> coke::Task<void> {
        try {
            co_await store.update(sess_id, 1, patch_b, std::chrono::system_clock::now() + std::chrono::seconds(1800));
        } catch (const kale::SessionConflict &) {
            conflict_caught = true;
        }
    }());
    CHECK(conflict_caught);

    // Store record preserves Request A's data and version 2
    auto store_check = coke::sync_wait(store.load(sess_id));
    REQUIRE(store_check.has_value());
    CHECK_EQ(store_check->version, 2);
    CHECK_EQ(store_check->values.at("counter"), "1");
}

// ----------------------------------------------------------------------------
// 10. Idempotent Destroy
// ----------------------------------------------------------------------------
TEST_CASE("Session: idempotent destroy") {
    kale::MemorySessionStore store;
    SessionServerFixture fixture;
    fixture->use(kale::session(store));

    fixture->POST("/create", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        co_await sess.create({{"user", "charlie"}});
        ctx.string(sess.id());
    });

    fixture->POST("/logout-double", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK(sess.exists());
        co_await sess.destroy();
        CHECK_FALSE(sess.exists());
        CHECK(sess.id().empty());

        // Second call must succeed idempotently without error
        co_await sess.destroy();
        CHECK_FALSE(sess.exists());
        ctx.string("destroyed");
    });

    fixture->POST("/destroy-anon-raw", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        // Destroy with raw cookie id supplied
        co_await sess.destroy();
        ctx.string("anon_destroyed");
    });

    auto create_resp = fixture.post("/create");
    CHECK(create_resp.ok());
    std::string sess_id = create_resp.body;

    auto logout_resp = fixture.post("/logout-double", "", {{"Cookie", "kale_session=" + sess_id}});
    CHECK(logout_resp.ok());
    CHECK_EQ(logout_resp.body, "destroyed");
    REQUIRE_FALSE(logout_resp.set_cookies.empty());
    // Clearing cookie has Max-Age=0
    CHECK(logout_resp.set_cookies[0].find("Max-Age=0") != std::string::npos);

    // Store record is erased
    auto check = coke::sync_wait(store.load(sess_id));
    CHECK_FALSE(check.has_value());

    // Destroy anonymous with raw cookie
    auto raw_anon_resp = fixture.post("/destroy-anon-raw", "", {{"Cookie", "kale_session=unknown_raw_cookie"}});
    CHECK(raw_anon_resp.ok());
    REQUIRE_FALSE(raw_anon_resp.set_cookies.empty());
    CHECK(raw_anon_resp.set_cookies[0].find("Max-Age=0") != std::string::npos);
}

// ----------------------------------------------------------------------------
// 11. Store Failure Propagation
// ----------------------------------------------------------------------------
TEST_CASE("Session: store failure propagation leaves Cookie and snapshot untouched") {
    auto failing_store = std::make_shared<FailingStore>();
    failing_store->fail_insert = true;

    SessionServerFixture fixture;
    fixture->use(kale::session(*failing_store));

    fixture->POST("/try-create", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        CHECK_FALSE(sess.exists());
        try {
            co_await sess.create({{"k", "v"}});
        } catch (const kale::SessionStoreError &) {
            // Snapshot remains untouched
            CHECK_FALSE(sess.exists());
            CHECK(sess.id().empty());
            ctx.set_status(500).string("store_failed");
            co_return;
        }
        ctx.string("unexpected_success");
    });

    auto resp = fixture.post("/try-create");
    CHECK_EQ(resp.status, 500);
    CHECK_EQ(resp.body, "store_failed");
    // Response MUST NOT contain any Set-Cookie header!
    CHECK(resp.set_cookies.empty());
}

// ----------------------------------------------------------------------------
// 12. SSE Pre-Send Renew and Post-Send SessionWriteAfterResponse
// ----------------------------------------------------------------------------
TEST_CASE("Session: SSE pre-send renew and post-send SessionWriteAfterResponse with zero Store writes") {
    auto counting_store = std::make_shared<CountingStore>();
    std::string active_id = "sse_active_session_1234567890abcdef12345678901234567890abcdef1234";
    kale::SessionRecord initial_rec;
    initial_rec.values["user"] = "streamer";
    initial_rec.version = 1;
    initial_rec.expires_at = std::chrono::system_clock::now() + std::chrono::seconds(1800);
    coke::sync_wait(counting_store->insert(active_id, initial_rec));

    SessionServerFixture fixture;
    fixture->use(kale::session(*counting_store));

    std::atomic<bool> pre_send_renew_ok{false};
    std::atomic<bool> post_send_caught_all{false};

    fixture->GET("/sse-session", [&](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        // Pre-send renew: response is uncommitted, renew succeeds!
        co_await sess.renew();
        pre_send_renew_ok = true;

        // Start SSE stream (headers committed)
        kale::SseEmitter sse(ctx);
        co_await sse.send("connected");

        // Post-send mutation attempts MUST all throw SessionWriteAfterResponse
        int caught_count = 0;
        try {
            co_await sess.set("k", "v");
        } catch (const kale::SessionWriteAfterResponse &) {
            caught_count++;
        }
        try {
            co_await sess.erase("user");
        } catch (const kale::SessionWriteAfterResponse &) {
            caught_count++;
        }
        try {
            co_await sess.renew();
        } catch (const kale::SessionWriteAfterResponse &) {
            caught_count++;
        }
        try {
            co_await sess.destroy();
        } catch (const kale::SessionWriteAfterResponse &) {
            caught_count++;
        }
        try {
            co_await sess.regenerate();
        } catch (const kale::SessionWriteAfterResponse &) {
            caught_count++;
        }
        try {
            co_await sess.create();
        } catch (const kale::SessionWriteAfterResponse &) {
            caught_count++;
        }

        if (caught_count == 6) {
            post_send_caught_all = true;
        }
    });

    int writes_before = counting_store->update_count.load() +
                        counting_store->insert_count.load() +
                        counting_store->replace_count.load() +
                        counting_store->destroy_count.load();

    auto resp = fixture.get("/sse-session", {{"Cookie", "kale_session=" + active_id}});
    CHECK(resp.ok());
    CHECK(pre_send_renew_ok.load());
    CHECK(post_send_caught_all.load());

    int writes_after = counting_store->update_count.load() +
                       counting_store->insert_count.load() +
                       counting_store->replace_count.load() +
                       counting_store->destroy_count.load();

    // The pre-send renew made 1 update call.
    // The 6 post-send attempts MUST NOT have made any store write calls!
    CHECK_EQ(writes_after, writes_before + 1);
}

// ----------------------------------------------------------------------------
// 13. RAII Attachment Unbind After next()
// ----------------------------------------------------------------------------
TEST_CASE("Session: RAII attachment unbind after next()") {
    kale::MemorySessionStore store;
    SessionServerFixture fixture;

    std::atomic<bool> inside_next_ok{false};
    std::atomic<bool> after_next_unbound_ok{false};
    std::atomic<bool> after_exception_unbound_ok{false};

    // Outer observer middleware
    fixture->use([&](kale::Context &ctx, kale::Next next) -> coke::Task<void> {
        try {
            co_await next();
            // After successful next(), session attachment must be unbound!
            try {
                kale::get_session(ctx);
            } catch (const std::logic_error &) {
                after_next_unbound_ok = true;
            }
        } catch (const std::runtime_error &) {
            // After exception in next(), session attachment must be unbound!
            try {
                kale::get_session(ctx);
            } catch (const std::logic_error &) {
                after_exception_unbound_ok = true;
            }
        }
    });

    fixture->use(kale::session(store));

    fixture->GET("/normal", [&](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        inside_next_ok = !sess.exists();
        ctx.string("ok");
    });

    fixture->GET("/throw", [&](kale::Context &ctx) {
        auto &sess = kale::get_session(ctx);
        (void)sess;
        throw std::runtime_error("route_failed");
    });

    auto resp1 = fixture.get("/normal");
    CHECK(resp1.ok());
    CHECK(inside_next_ok.load());
    CHECK(after_next_unbound_ok.load());

    auto resp2 = fixture.get("/throw");
    CHECK(after_exception_unbound_ok.load());
}

// ----------------------------------------------------------------------------
// 14. Custom SessionStore Subclass Integration
// ----------------------------------------------------------------------------
TEST_CASE("Session: custom SessionStore subclass integration") {
    CustomUserStore custom_store;
    SessionServerFixture fixture;
    fixture->use(kale::session(custom_store));

    fixture->POST("/custom-flow", [](kale::Context &ctx) -> coke::Task<void> {
        auto &sess = kale::get_session(ctx);
        co_await sess.create({{"custom_key", "custom_val"}});
        CHECK_EQ(sess.get("custom_key"), "custom_val");

        co_await sess.set("custom_key2", "val2");
        CHECK_EQ(sess.get("custom_key2"), "val2");

        ctx.string(sess.id());
    });

    auto resp = fixture.post("/custom-flow");
    CHECK(resp.ok());
    std::string sess_id = resp.body;
    CHECK_EQ(sess_id.size(), 64);

    // Verify in custom_store
    auto rec = coke::sync_wait(custom_store.load(sess_id));
    REQUIRE(rec.has_value());
    CHECK_EQ(rec->values.at("custom_key"), "custom_val");
    CHECK_EQ(rec->values.at("custom_key2"), "val2");
}

// ----------------------------------------------------------------------------
// 15. MemorySessionStore Opportunistic Reclamation of Expired Records on Insert
// ----------------------------------------------------------------------------
TEST_CASE("Session: MemorySessionStore opportunistic reclamation on insert preserves live sessions") {
    kale::MemorySessionStore store;
    auto now = std::chrono::system_clock::now();

    const std::string live_id = "live_session_11111111111111111111111111111111111111111111111111111111";
    const std::string expired_id = "expired_session_222222222222222222222222222222222222222222222222222222";
    const std::string expired_id_2 = "expired_session_333333333333333333333333333333333333333333333333333333";
    const std::string fresh_id = "fresh_session_4444444444444444444444444444444444444444444444444444444";

    // 1. Seed an active session with expiration in the future
    kale::SessionRecord live_rec;
    live_rec.values["user"] = "bob";
    live_rec.version = 1;
    live_rec.expires_at = now + std::chrono::seconds(1800);
    coke::sync_wait(store.insert(live_id, std::move(live_rec)));

    // 2. Seed abandoned sessions with controlled expiration in the past
    kale::SessionRecord expired_rec_1;
    expired_rec_1.values["user"] = "alice";
    expired_rec_1.version = 1;
    expired_rec_1.expires_at = now - std::chrono::seconds(60);
    coke::sync_wait(store.insert(expired_id, std::move(expired_rec_1)));

    kale::SessionRecord expired_rec_2;
    expired_rec_2.values["user"] = "dave";
    expired_rec_2.version = 1;
    expired_rec_2.expires_at = now - std::chrono::seconds(120);
    coke::sync_wait(store.insert(expired_id_2, std::move(expired_rec_2)));

    // 3. Insert a distinct fresh session; this triggers the opportunistic sweep
    kale::SessionRecord fresh_rec;
    fresh_rec.values["user"] = "charlie";
    fresh_rec.version = 1;
    fresh_rec.expires_at = now + std::chrono::seconds(1800);
    coke::sync_wait(store.insert(fresh_id, std::move(fresh_rec)));

    // 4. Verify that the unrelated live session is preserved and still loadable
    auto loaded_live = coke::sync_wait(store.load(live_id));
    REQUIRE(loaded_live.has_value());
    CHECK_EQ(loaded_live->values.at("user"), "bob");
    CHECK_EQ(loaded_live->version, 1);
    CHECK(loaded_live->expires_at > now);

    // Verify that the fresh session is loadable
    auto loaded_fresh = coke::sync_wait(store.load(fresh_id));
    REQUIRE(loaded_fresh.has_value());
    CHECK_EQ(loaded_fresh->values.at("user"), "charlie");
    CHECK_EQ(loaded_fresh->version, 1);

    // 5. Verify state behavior of the expired records:
    // Loading the expired session returns nullopt
    auto loaded_expired = coke::sync_wait(store.load(expired_id));
    CHECK(!loaded_expired.has_value());

    // Updating the expired record throws SessionMissing with "session missing" (not "session expired"),
    // demonstrating it was already reclaimed during insert rather than found and lazily dropped during update.
    bool update_missing_caught = false;
    kale::SessionPatch patch;
    patch.set["user"] = "alice_new";
    coke::sync_wait([&]() -> coke::Task<void> {
        try {
            co_await store.update(expired_id, 1, patch, now + std::chrono::seconds(1800));
        } catch (const kale::SessionMissing &ex) {
            update_missing_caught = true;
            std::string msg = ex.what();
            CHECK_NE(msg.find("session missing"), std::string::npos);
            CHECK_EQ(msg.find("session expired"), std::string::npos);
        }
    }());
    CHECK(update_missing_caught);

    // Replacing expired_id_2 throws SessionMissing with "old session missing" (not "old session expired")
    bool replace_missing_caught = false;
    kale::SessionRecord rep_rec;
    rep_rec.values["user"] = "dave_rep";
    rep_rec.version = 1;
    rep_rec.expires_at = now + std::chrono::seconds(1800);
    coke::sync_wait([&]() -> coke::Task<void> {
        try {
            co_await store.replace(expired_id_2, 1, "some_new_id", std::move(rep_rec));
        } catch (const kale::SessionMissing &ex) {
            replace_missing_caught = true;
            std::string msg = ex.what();
            CHECK_NE(msg.find("old session missing"), std::string::npos);
            CHECK_EQ(msg.find("old session expired"), std::string::npos);
        }
    }());
    CHECK(replace_missing_caught);

    // 6. Verify that inserting under the active live ID raises SessionConflict
    kale::SessionRecord duplicate_live_rec;
    duplicate_live_rec.values["user"] = "duplicate";
    duplicate_live_rec.version = 1;
    duplicate_live_rec.expires_at = now + std::chrono::seconds(1800);
    bool conflict_caught = false;
    coke::sync_wait([&]() -> coke::Task<void> {
        try {
            co_await store.insert(live_id, std::move(duplicate_live_rec));
        } catch (const kale::SessionConflict &) {
            conflict_caught = true;
        }
    }());
    CHECK(conflict_caught);

    // 7. Verify that the reclaimed expired ID can now be cleanly re-inserted without conflict
    kale::SessionRecord reused_id_rec;
    reused_id_rec.values["user"] = "alice_revived";
    reused_id_rec.version = 1;
    reused_id_rec.expires_at = now + std::chrono::seconds(1800);
    coke::sync_wait(store.insert(expired_id, std::move(reused_id_rec)));

    auto reloaded_reused = coke::sync_wait(store.load(expired_id));
    REQUIRE(reloaded_reused.has_value());
    CHECK_EQ(reloaded_reused->values.at("user"), "alice_revived");
}

} // TEST_SUITE("core")
