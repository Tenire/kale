#include "kale/security/session.h"
#include "kale/core/cookie.h"
#include "security/crypto_util.h"

#include <limits>

#include <cctype>
#include <chrono>
#include <utility>

#include "workflow/HttpMessage.h"
#include "workflow/HttpUtil.h"

namespace kale {

namespace {

bool header_name_matches(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

void erase_response_cookie(protocol::HttpResponse &resp, std::string_view cookie_name) {
    protocol::HttpHeaderCursor cursor(&resp);
    protocol::HttpMessageHeader h;
    while (cursor.next(&h)) {
        std::string_view name(static_cast<const char*>(h.name), h.name_len);
        if (header_name_matches(name, "Set-Cookie")) {
            std::string_view val(static_cast<const char*>(h.value), h.value_len);
            if (val.starts_with(cookie_name) && val.size() > cookie_name.size() && val[cookie_name.size()] == '=') {
                cursor.erase();
            }
        }
    }
}

static void validate_session_options(const SessionOptions &options) {
    if (options.ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("SessionOptions::ttl must be positive");
    }
    if (options.ttl > std::chrono::seconds(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("SessionOptions::ttl exceeds int capacity");
    }
    if (options.cookie_path.empty() || options.cookie_path.front() != '/') {
        throw std::invalid_argument("SessionOptions::cookie_path must start with '/'");
    }
    if (options.same_site == SessionSameSite::None && !options.secure) {
        throw std::invalid_argument("SessionOptions: SameSite=None requires Secure=true");
    }
    Cookie sample(options.cookie_name, "valid_value");
    sample.set_path(options.cookie_path);
    if (options.cookie_domain) {
        sample.set_domain(*options.cookie_domain);
    }
    if (!sample.is_valid()) {
        throw std::invalid_argument("SessionOptions contains invalid cookie name, path, or domain");
    }
}

Cookie::SameSite to_cookie_same_site(SessionSameSite ss) noexcept {
    switch (ss) {
        case SessionSameSite::Lax: return Cookie::SameSite::Lax;
        case SessionSameSite::Strict: return Cookie::SameSite::Strict;
        case SessionSameSite::None: return Cookie::SameSite::None;
    }
    return Cookie::SameSite::Lax;
}

} // namespace


coke::Task<std::optional<SessionRecord>> MemorySessionStore::load(std::string id) {
    std::optional<SessionRecord> res;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = records_.find(id);
        if (it != records_.end()) {
            if (it->second.expires_at <= std::chrono::system_clock::now()) {
                records_.erase(it);
            } else {
                res = it->second;
            }
        }
    }
    co_return res;
}

coke::Task<void> MemorySessionStore::insert(std::string id, SessionRecord record) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now = std::chrono::system_clock::now();

        std::erase_if(records_, [now](const auto &item) {
            return item.second.expires_at <= now;
        });

        auto it = records_.find(id);
        if (it != records_.end()) {
            throw SessionConflict("MemorySessionStore::insert: session id already exists: " + id);
        }
        if (record.version == 0) {
            record.version = 1;
        }
        records_.emplace(std::move(id), std::move(record));
    }
    co_return;
}

coke::Task<SessionRecord> MemorySessionStore::update(
    std::string id,
    std::uint64_t expected_version,
    SessionPatch patch,
    std::chrono::system_clock::time_point expires_at) {

    for (const auto &k : patch.erase) {
        if (patch.set.find(k) != patch.set.end()) {
            throw std::invalid_argument("MemorySessionStore::update: key present in both patch set and erase: " + k);
        }
    }

    SessionRecord res;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now = std::chrono::system_clock::now();
        auto it = records_.find(id);
        if (it == records_.end()) {
            throw SessionMissing("MemorySessionStore::update: session missing: " + id);
        }
        if (it->second.expires_at <= now) {
            records_.erase(it);
            throw SessionMissing("MemorySessionStore::update: session expired: " + id);
        }
        if (it->second.version != expected_version) {
            throw SessionConflict("MemorySessionStore::update: version mismatch, expected " +
                                  std::to_string(expected_version) + ", actual " +
                                  std::to_string(it->second.version));
        }

        for (const auto &k : patch.erase) {
            it->second.values.erase(k);
        }
        for (auto &p : patch.set) {
            it->second.values[std::move(p.first)] = std::move(p.second);
        }
        it->second.version = expected_version + 1;
        it->second.expires_at = expires_at;
        res = it->second;
    }
    co_return res;
}

coke::Task<void> MemorySessionStore::replace(
    std::string old_id,
    std::uint64_t expected_version,
    std::string new_id,
    SessionRecord replacement) {

    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto now = std::chrono::system_clock::now();
        auto old_it = records_.find(old_id);
        if (old_it == records_.end()) {
            throw SessionMissing("MemorySessionStore::replace: old session missing: " + old_id);
        }
        if (old_it->second.expires_at <= now) {
            records_.erase(old_it);
            throw SessionMissing("MemorySessionStore::replace: old session expired: " + old_id);
        }
        if (old_it->second.version != expected_version) {
            throw SessionConflict("MemorySessionStore::replace: version mismatch, expected " +
                                  std::to_string(expected_version) + ", actual " +
                                  std::to_string(old_it->second.version));
        }

        if (new_id != old_id) {
            auto new_it = records_.find(new_id);
            if (new_it != records_.end()) {
                if (new_it->second.expires_at > now) {
                    throw SessionConflict("MemorySessionStore::replace: new session id already exists: " + new_id);
                }
            }
        }

        if (replacement.version == 0) {
            replacement.version = 1;
        }

        if (new_id != old_id) {
            records_.erase(old_it);
            records_[std::move(new_id)] = std::move(replacement);
        } else {
            old_it->second = std::move(replacement);
        }
    }
    co_return;
}

coke::Task<void> MemorySessionStore::destroy(std::string id) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        records_.erase(id);
    }
    co_return;
}

// ----------------------------------------------------------------------------
// Session Implementation
// ----------------------------------------------------------------------------

Session::Session(Context &ctx, SessionStore &store, SessionOptions options, std::string raw_cookie_id)
    : ctx_(ctx),
      store_(store),
      options_(std::move(options)),
      raw_cookie_id_(std::move(raw_cookie_id)) {
    validate_session_options(options_);
}

bool Session::exists() const noexcept {
    return exists_;
}

bool Session::contains(std::string_view key) const {
    if (!exists_) return false;
    return values_.find(key) != values_.end();
}

std::optional<std::string> Session::get(std::string_view key) const {
    if (!exists_) return std::nullopt;
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::uint64_t Session::version() const noexcept {
    return version_;
}

std::string_view Session::id() const noexcept {
    return id_;
}

void Session::init_active(std::string id, SessionRecord record) {
    exists_ = true;
    id_ = std::move(id);
    raw_cookie_id_ = id_;
    version_ = record.version;
    values_ = std::move(record.values);
    expires_at_ = record.expires_at;
}

void Session::check_not_committed() const {
    if (ctx_.is_stream() || ctx_.is_replied()) {
        throw SessionWriteAfterResponse("Cannot mutate session after response headers sent or committed");
    }
}

void Session::set_response_cookie(std::string_view val, std::chrono::seconds ttl) {
    Cookie cookie(options_.cookie_name, std::string(val));
    cookie.set_path(options_.cookie_path);
    if (options_.cookie_domain) {
        cookie.set_domain(*options_.cookie_domain);
    }
    cookie.set_http_only(options_.http_only);
    cookie.set_secure(options_.secure);
    cookie.set_same_site(to_cookie_same_site(options_.same_site));
    cookie.set_max_age(static_cast<int>(ttl.count()));

    erase_response_cookie(ctx_.raw_resp(), options_.cookie_name);
    ctx_.set_cookie(std::move(cookie));
}

void Session::clear_response_cookie() {
    Cookie cookie(options_.cookie_name, "");
    cookie.set_path(options_.cookie_path);
    if (options_.cookie_domain) {
        cookie.set_domain(*options_.cookie_domain);
    }
    cookie.set_http_only(options_.http_only);
    cookie.set_secure(options_.secure);
    cookie.set_same_site(to_cookie_same_site(options_.same_site));
    cookie.set_max_age(0);

    erase_response_cookie(ctx_.raw_resp(), options_.cookie_name);
    ctx_.set_cookie(std::move(cookie));
}

coke::Task<void> Session::create(SessionValuesInit values_init) {
    SessionValues values = std::move(values_init.values);
    check_not_committed();
    if (exists_) {
        throw SessionConflict("Session::create: active session already exists on request");
    }

    constexpr int kMaxRetries = 3;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        std::string new_id;
        try {
            new_id = CryptoUtil::random_bytes_hex(32);
        } catch (const std::exception &ex) {
            throw SessionStoreError(std::string("Session::create CSPRNG failure: ") + ex.what());
        }
        if (new_id.empty()) {
            throw SessionStoreError("Session::create: CSPRNG returned empty id");
        }

        auto now = std::chrono::system_clock::now();
        SessionRecord record{values, 1, now + options_.ttl};

        try {
            co_await store_.insert(new_id, record);
            id_ = std::move(new_id);
            raw_cookie_id_ = id_;
            version_ = 1;
            values_ = std::move(record.values);
            expires_at_ = record.expires_at;
            exists_ = true;
            set_response_cookie(id_, options_.ttl);
            co_return;
        } catch (const SessionConflict &) {
            if (attempt == kMaxRetries - 1) {
                throw;
            }
        }
    }
}

coke::Task<void> Session::update(SessionPatch patch) {
    check_not_committed();
    for (const auto &k : patch.erase) {
        if (patch.set.find(k) != patch.set.end()) {
            throw std::invalid_argument("Session::update: key present in both patch set and erase: " + k);
        }
    }
    if (!exists_) {
        throw SessionMissing("Session::update: session does not exist");
    }

    auto now = std::chrono::system_clock::now();
    SessionRecord updated = co_await store_.update(id_, version_, std::move(patch), now + options_.ttl);
    values_ = std::move(updated.values);
    version_ = updated.version;
    expires_at_ = updated.expires_at;
}

coke::Task<void> Session::set(std::string key, std::string value) {
    SessionPatch patch;
    patch.set.emplace(std::move(key), std::move(value));
    co_await update(std::move(patch));
}

coke::Task<void> Session::erase(std::string key) {
    SessionPatch patch;
    patch.erase.push_back(std::move(key));
    co_await update(std::move(patch));
}

coke::Task<void> Session::renew() {
    check_not_committed();
    if (!exists_) {
        throw SessionMissing("Session::renew: session does not exist");
    }

    auto now = std::chrono::system_clock::now();
    SessionRecord updated = co_await store_.update(id_, version_, SessionPatch{}, now + options_.ttl);
    values_ = std::move(updated.values);
    version_ = updated.version;
    expires_at_ = updated.expires_at;
    set_response_cookie(id_, options_.ttl);
}

coke::Task<void> Session::regenerate(SessionValuesInit replacement_init) {
    SessionValues replacement = std::move(replacement_init.values);
    check_not_committed();

    constexpr int kMaxRetries = 3;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        std::string new_id;
        try {
            new_id = CryptoUtil::random_bytes_hex(32);
        } catch (const std::exception &ex) {
            throw SessionStoreError(std::string("Session::regenerate CSPRNG failure: ") + ex.what());
        }
        if (new_id.empty()) {
            throw SessionStoreError("Session::regenerate: CSPRNG returned empty id");
        }

        auto now = std::chrono::system_clock::now();
        SessionRecord record{replacement, 1, now + options_.ttl};

        try {
            if (exists_) {
                co_await store_.replace(id_, version_, new_id, record);
            } else {
                co_await store_.insert(new_id, record);
            }
            id_ = std::move(new_id);
            raw_cookie_id_ = id_;
            version_ = 1;
            values_ = std::move(record.values);
            expires_at_ = record.expires_at;
            exists_ = true;
            set_response_cookie(id_, options_.ttl);
            co_return;
        } catch (const SessionConflict &) {
            if (attempt == kMaxRetries - 1) {
                throw;
            }
        }
    }
}

coke::Task<void> Session::destroy() {
    check_not_committed();
    if (exists_) {
        co_await store_.destroy(id_);
    } else if (!raw_cookie_id_.empty()) {
        co_await store_.destroy(raw_cookie_id_);
    }
    exists_ = false;
    id_.clear();
    raw_cookie_id_.clear();
    version_ = 0;
    values_.clear();
    expires_at_ = {};
    clear_response_cookie();
}

// ----------------------------------------------------------------------------
// get_session & session Middleware
// ----------------------------------------------------------------------------

namespace {
struct SessionAttachment {
    Session *ptr{nullptr};
};
} // namespace

Session &get_session(Context &ctx) {
    auto *att = ctx.get_attachment<SessionAttachment>();
    if (!att || !att->ptr) {
        throw std::logic_error("kale::get_session: Session middleware not present on Context or already unbound");
    }
    return *att->ptr;
}

const Session &get_session(const Context &ctx) {
    const auto *att = ctx.get_attachment<SessionAttachment>();
    if (!att || !att->ptr) {
        throw std::logic_error("kale::get_session: Session middleware not present on Context or already unbound");
    }
    return *att->ptr;
}

Middleware session(SessionStore &store, SessionOptions options) {
    validate_session_options(options);

    return [store_ptr = &store, opts = std::move(options)](Context &ctx, Next next) -> coke::Task<void> {
        std::string_view raw_id_view = ctx.cookie(opts.cookie_name);
        std::string raw_id(raw_id_view);

        Session session_obj(ctx, *store_ptr, opts, raw_id);

        if (!raw_id.empty()) {
            std::optional<SessionRecord> rec = co_await store_ptr->load(raw_id);
            if (rec.has_value()) {
                auto now = std::chrono::system_clock::now();
                if (rec->expires_at > now) {
                    session_obj.init_active(raw_id, std::move(*rec));
                }
            }
        }

        ctx.set_attachment<SessionAttachment>(SessionAttachment{&session_obj});

        struct AttachmentGuard {
            Context &c;
            ~AttachmentGuard() {
                c.erase_attachment<SessionAttachment>();
            }
        } guard{ctx};

        co_await next();
    };
}

} // namespace kale
