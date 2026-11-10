#ifndef KALE_EVP_CHECKSUM_H
#define KALE_EVP_CHECKSUM_H

#include <memory>
#include <string>
#include <string_view>

#include "kale/core/checksum.h"

struct evp_md_ctx_st;
struct evp_md_st;

namespace kale {

struct EvpMdCtxDeleter {
    void operator()(evp_md_ctx_st *ctx) const noexcept;
};

struct EvpMdDeleter {
    void operator()(evp_md_st *md) const noexcept;
};

/**
 * Checksum verifier using OpenSSL EVP message digests.
 * Failures invalidate the digest context until reset().
 */
class EvpChecksum : public ChecksumVerifier {
public:
    using ChecksumVerifier::update;

    /**
     * @throws std::invalid_argument if algorithm_name is empty, contains NUL, or is unsupported.
     */
    explicit EvpChecksum(std::string_view algorithm_name = "SHA256");

    ~EvpChecksum() override;

    EvpChecksum(const EvpChecksum &) = delete;
    EvpChecksum &operator=(const EvpChecksum &) = delete;

    EvpChecksum(EvpChecksum &&other) noexcept;
    EvpChecksum &operator=(EvpChecksum &&other) noexcept;

    void reset() override;
    void update(const void *buf, size_t len) override;
    std::string hex_digest() override;

    const std::string &algorithm_name() const noexcept { return algorithm_name_; }
    bool is_finalized() const noexcept { return finalized_; }

private:
    const evp_md_st *md() const noexcept;

    std::string algorithm_name_;
    std::unique_ptr<evp_md_st, EvpMdDeleter> fetched_md_;
    const evp_md_st *static_md_{nullptr};
    std::unique_ptr<evp_md_ctx_st, EvpMdCtxDeleter> ctx_;
    bool finalized_{false};
    std::string cached_digest_;
};

} // namespace kale

#endif // KALE_EVP_CHECKSUM_H
