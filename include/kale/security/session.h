#ifndef KALE_SESSION_H
#define KALE_SESSION_H

#include <chrono>
#include <concepts>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <utility>

#include "coke/task.h"
#include "kale/core/context.h"
#include "kale/core/router.h"

namespace kale {

// ----------------------------------------------------------------------------
// Public Types
// ----------------------------------------------------------------------------

using SessionValues = std::map<std::string, std::string, std::less<>>;

struct SessionEntry {
    std::string key;
    std::string value;
};

struct SessionValuesInit {
    SessionValues values;

    SessionValuesInit() = default;
    template <typename Map>
        requires std::same_as<std::remove_cvref_t<Map>, SessionValues>
    SessionValuesInit(Map &&v) noexcept : values(std::forward<Map>(v)) {}

    SessionValuesInit(SessionEntry e1) {
        values.insert_or_assign(std::move(e1.key), std::move(e1.value));
    }
    SessionValuesInit(SessionEntry e1, SessionEntry e2) {
        values.insert_or_assign(std::move(e1.key), std::move(e1.value));
        values.insert_or_assign(std::move(e2.key), std::move(e2.value));
    }
    SessionValuesInit(SessionEntry e1, SessionEntry e2, SessionEntry e3) {
        values.insert_or_assign(std::move(e1.key), std::move(e1.value));
        values.insert_or_assign(std::move(e2.key), std::move(e2.value));
        values.insert_or_assign(std::move(e3.key), std::move(e3.value));
    }
    SessionValuesInit(SessionEntry e1, SessionEntry e2, SessionEntry e3, SessionEntry e4) {
        values.insert_or_assign(std::move(e1.key), std::move(e1.value));
        values.insert_or_assign(std::move(e2.key), std::move(e2.value));
        values.insert_or_assign(std::move(e3.key), std::move(e3.value));
        values.insert_or_assign(std::move(e4.key), std::move(e4.value));
    }
    SessionValuesInit(SessionEntry e1, SessionEntry e2, SessionEntry e3, SessionEntry e4,
                      SessionEntry e5, SessionEntry e6 = {}, SessionEntry e7 = {}, SessionEntry e8 = {}) {
        values.insert_or_assign(std::move(e1.key), std::move(e1.value));
        values.insert_or_assign(std::move(e2.key), std::move(e2.value));
        values.insert_or_assign(std::move(e3.key), std::move(e3.value));
        values.insert_or_assign(std::move(e4.key), std::move(e4.value));
        values.insert_or_assign(std::move(e5.key), std::move(e5.value));
        if (!e6.key.empty()) values.insert_or_assign(std::move(e6.key), std::move(e6.value));
        if (!e7.key.empty()) values.insert_or_assign(std::move(e7.key), std::move(e7.value));
        if (!e8.key.empty()) values.insert_or_assign(std::move(e8.key), std::move(e8.value));
    }

    operator SessionValues() && noexcept { return std::move(values); }
    operator SessionValues() const & { return values; }
};

struct SessionPatch {
    SessionValues set;
    std::vector<std::string> erase;
};

enum class SessionSameSite { Lax, Strict, None };

struct SessionOptions {
    std::string cookie_name{"kale_session"};
    std::string cookie_path{"/"};
    std::optional<std::string> cookie_domain;
    bool http_only{true};
    bool secure{true};
    SessionSameSite same_site{SessionSameSite::Lax};
    std::chrono::seconds ttl{1800};
};

struct SessionRecord {
    SessionValues values;
    std::uint64_t version{1};
    std::chrono::system_clock::time_point expires_at{};
};

// ----------------------------------------------------------------------------
// Exception Hierarchy
// ----------------------------------------------------------------------------

class SessionError : public std::runtime_error {
public:
    explicit SessionError(const std::string &what_arg) : std::runtime_error(what_arg) {}
    explicit SessionError(const char *what_arg) : std::runtime_error(what_arg) {}
};

class SessionMissing : public SessionError {
public:
    explicit SessionMissing(const std::string &what_arg) : SessionError(what_arg) {}
    explicit SessionMissing(const char *what_arg) : SessionError(what_arg) {}
};

class SessionConflict : public SessionError {
public:
    explicit SessionConflict(const std::string &what_arg) : SessionError(what_arg) {}
    explicit SessionConflict(const char *what_arg) : SessionError(what_arg) {}
};

class SessionStoreError : public SessionError {
public:
    explicit SessionStoreError(const std::string &what_arg) : SessionError(what_arg) {}
    explicit SessionStoreError(const char *what_arg) : SessionError(what_arg) {}
};

class SessionWriteAfterResponse : public SessionError {
public:
    explicit SessionWriteAfterResponse(const std::string &what_arg) : SessionError(what_arg) {}
    explicit SessionWriteAfterResponse(const char *what_arg) : SessionError(what_arg) {}
};

// ----------------------------------------------------------------------------
// SessionStore & MemorySessionStore
// ----------------------------------------------------------------------------

class SessionStore {
public:
    virtual ~SessionStore() = default;
    virtual coke::Task<std::optional<SessionRecord>> load(std::string id) = 0;
    virtual coke::Task<void> insert(std::string id, SessionRecord record) = 0;
    virtual coke::Task<SessionRecord> update(
        std::string id,
        std::uint64_t expected_version,
        SessionPatch patch,
        std::chrono::system_clock::time_point expires_at) = 0;
    virtual coke::Task<void> replace(
        std::string old_id,
        std::uint64_t expected_version,
        std::string new_id,
        SessionRecord replacement) = 0;
    virtual coke::Task<void> destroy(std::string id) = 0;
};

class MemorySessionStore final : public SessionStore {
public:
    MemorySessionStore() = default;
    ~MemorySessionStore() override = default;

    coke::Task<std::optional<SessionRecord>> load(std::string id) override;
    coke::Task<void> insert(std::string id, SessionRecord record) override;
    coke::Task<SessionRecord> update(
        std::string id,
        std::uint64_t expected_version,
        SessionPatch patch,
        std::chrono::system_clock::time_point expires_at) override;
    coke::Task<void> replace(
        std::string old_id,
        std::uint64_t expected_version,
        std::string new_id,
        SessionRecord replacement) override;
    coke::Task<void> destroy(std::string id) override;

private:
    std::mutex mtx_;
    std::unordered_map<std::string, SessionRecord> records_;
};

// ----------------------------------------------------------------------------
// Session
// ----------------------------------------------------------------------------

class Session {
public:
    Session(Context &ctx, SessionStore &store, SessionOptions options, std::string raw_cookie_id = "");
    ~Session() = default;

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Session(Session &&) = delete;
    Session &operator=(Session &&) = delete;

    bool exists() const noexcept;
    bool contains(std::string_view key) const;
    std::optional<std::string> get(std::string_view key) const;
    std::uint64_t version() const noexcept;
    std::string_view id() const noexcept;
    const SessionValues &values() const noexcept { return values_; }

    coke::Task<void> create(SessionValuesInit values = {});
    coke::Task<void> update(SessionPatch patch);
    coke::Task<void> set(std::string key, std::string value);
    coke::Task<void> erase(std::string key);
    coke::Task<void> renew();
    coke::Task<void> regenerate(SessionValuesInit replacement = {});
    coke::Task<void> destroy();

    void init_active(std::string id, SessionRecord record);

private:
    void check_not_committed() const;
    void set_response_cookie(std::string_view id, std::chrono::seconds ttl);
    void clear_response_cookie();

    Context &ctx_;
    SessionStore &store_;
    SessionOptions options_;
    std::string raw_cookie_id_;
    bool exists_{false};
    std::string id_;
    std::uint64_t version_{0};
    SessionValues values_;
    std::chrono::system_clock::time_point expires_at_{};
};

Session &get_session(Context &ctx);
const Session &get_session(const Context &ctx);

Middleware session(SessionStore &store, SessionOptions options = SessionOptions{});

} // namespace kale

#endif // KALE_SESSION_H
