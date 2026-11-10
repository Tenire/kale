#include "kale/security/evp_checksum.h"

#include <openssl/evp.h>

#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace kale {

void EvpMdCtxDeleter::operator()(evp_md_ctx_st *ctx) const noexcept {
    if (ctx) {
        EVP_MD_CTX_free(reinterpret_cast<EVP_MD_CTX *>(ctx));
    }
}

void EvpMdDeleter::operator()(evp_md_st *md) const noexcept {
    if (md) {
        EVP_MD_free(reinterpret_cast<EVP_MD *>(md));
    }
}

EvpChecksum::EvpChecksum(std::string_view algorithm_name) {
    if (algorithm_name.empty()) {
        throw std::invalid_argument("EvpChecksum: digest algorithm name cannot be empty");
    }
    if (algorithm_name.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("EvpChecksum: algorithm name cannot contain embedded NUL characters");
    }

    algorithm_name_ = std::string(algorithm_name);

    // Fast-path resolution for common standard algorithms
    if (algorithm_name_ == "SHA256" || algorithm_name_ == "sha256") {
        static_md_ = reinterpret_cast<const evp_md_st *>(EVP_sha256());
    } else if (algorithm_name_ == "MD5" || algorithm_name_ == "md5") {
        static_md_ = reinterpret_cast<const evp_md_st *>(EVP_md5());
    } else if (algorithm_name_ == "SHA512" || algorithm_name_ == "sha512") {
        static_md_ = reinterpret_cast<const evp_md_st *>(EVP_sha512());
    } else {
        // Provider lookup via EVP_MD_fetch
        evp_md_st *fetched = reinterpret_cast<evp_md_st *>(
            EVP_MD_fetch(nullptr, algorithm_name_.c_str(), nullptr));
        if (fetched) {
            fetched_md_.reset(fetched);
        }
    }

    if (!md()) {
        throw std::invalid_argument("EvpChecksum: unknown or unsupported EVP digest algorithm: " + algorithm_name_);
    }

    EVP_MD_CTX *raw_ctx = EVP_MD_CTX_new();
    if (!raw_ctx) {
        throw std::bad_alloc();
    }
    ctx_.reset(reinterpret_cast<evp_md_ctx_st *>(raw_ctx));

    if (EVP_DigestInit_ex(reinterpret_cast<EVP_MD_CTX *>(ctx_.get()),
                          reinterpret_cast<const EVP_MD *>(md()),
                          nullptr) != 1) {
        ctx_.reset();
        throw std::runtime_error("EvpChecksum: EVP_DigestInit_ex failed for algorithm: " + algorithm_name_);
    }
}

EvpChecksum::~EvpChecksum() = default;

EvpChecksum::EvpChecksum(EvpChecksum &&other) noexcept
    : algorithm_name_(std::move(other.algorithm_name_)),
      fetched_md_(std::move(other.fetched_md_)),
      static_md_(other.static_md_),
      ctx_(std::move(other.ctx_)),
      finalized_(other.finalized_),
      cached_digest_(std::move(other.cached_digest_)) {
    other.static_md_ = nullptr;
    other.finalized_ = false;
}

EvpChecksum &EvpChecksum::operator=(EvpChecksum &&other) noexcept {
    if (this != &other) {
        algorithm_name_ = std::move(other.algorithm_name_);
        fetched_md_ = std::move(other.fetched_md_);
        static_md_ = other.static_md_;
        ctx_ = std::move(other.ctx_);
        finalized_ = other.finalized_;
        cached_digest_ = std::move(other.cached_digest_);

        other.static_md_ = nullptr;
        other.finalized_ = false;
    }
    return *this;
}

const evp_md_st *EvpChecksum::md() const noexcept {
    return fetched_md_ ? fetched_md_.get() : static_md_;
}

void EvpChecksum::reset() {
    if (!md()) {
        throw std::runtime_error("EvpChecksum::reset: no valid digest algorithm configured");
    }

    if (!ctx_) {
        EVP_MD_CTX *raw_ctx = EVP_MD_CTX_new();
        if (!raw_ctx) {
            throw std::bad_alloc();
        }
        ctx_.reset(reinterpret_cast<evp_md_ctx_st *>(raw_ctx));
    }

    if (EVP_DigestInit_ex(reinterpret_cast<EVP_MD_CTX *>(ctx_.get()),
                          reinterpret_cast<const EVP_MD *>(md()),
                          nullptr) != 1) {
        ctx_.reset();
        finalized_ = false;
        cached_digest_.clear();
        throw std::runtime_error("EvpChecksum::reset: EVP_DigestInit_ex failed for algorithm: " + algorithm_name_);
    }

    finalized_ = false;
    cached_digest_.clear();
}

void EvpChecksum::update(const void *buf, size_t len) {
    if (len == 0) {
        return; // Zero-length is a no-op; preserves cached digest
    }
    if (finalized_) {
        throw std::logic_error("EvpChecksum::update: cannot update finalized digest; call reset() first");
    }
    if (buf == nullptr) {
        throw std::invalid_argument("EvpChecksum::update: received null buffer with nonzero length");
    }
    if (!ctx_) {
        throw std::runtime_error("EvpChecksum::update: digest context is uninitialized or invalidated");
    }
    if (EVP_DigestUpdate(reinterpret_cast<EVP_MD_CTX *>(ctx_.get()), buf, len) != 1) {
        ctx_.reset(); // Invalidate on update failure
        throw std::runtime_error("EvpChecksum::update: EVP_DigestUpdate failed for algorithm: " + algorithm_name_);
    }
}

std::string EvpChecksum::hex_digest() {
    if (finalized_) {
        return cached_digest_;
    }
    if (!ctx_) {
        throw std::runtime_error("EvpChecksum::hex_digest: digest context is uninitialized or invalidated");
    }

    int md_size = EVP_MD_get_size(reinterpret_cast<const EVP_MD *>(md()));
    if (md_size <= 0 || md_size > EVP_MAX_MD_SIZE) {
        throw std::runtime_error("EvpChecksum::hex_digest: invalid digest size for algorithm: " + algorithm_name_);
    }

    // Allocate digest storage before finalization
    size_t hex_len = static_cast<size_t>(md_size) * 2;
    cached_digest_.resize(hex_len);

    unsigned char md_value[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    if (EVP_DigestFinal_ex(reinterpret_cast<EVP_MD_CTX *>(ctx_.get()), md_value, &md_len) != 1) {
        ctx_.reset(); // Invalidate on failure
        cached_digest_.clear();
        throw std::runtime_error("EvpChecksum::hex_digest: EVP_DigestFinal_ex failed for algorithm: " + algorithm_name_);
    }

    if (static_cast<size_t>(md_len) * 2 != hex_len) {
        cached_digest_.resize(static_cast<size_t>(md_len) * 2);
    }

    static constexpr char kHexDigits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < md_len; ++i) {
        cached_digest_[i * 2]     = kHexDigits[(md_value[i] >> 4) & 0x0F];
        cached_digest_[i * 2 + 1] = kHexDigits[md_value[i] & 0x0F];
    }

    finalized_ = true;
    return cached_digest_;
}

} // namespace kale
