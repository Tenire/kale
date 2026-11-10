#ifndef KALE_CORE_CRC32_CHECKSUM_H
#define KALE_CORE_CRC32_CHECKSUM_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "kale/core/checksum.h"

namespace kale {

/**
 * CRC-32 checksum calculator using zlib crc32.
 * Produces an 8-character lowercase hex digest.
 */
class Crc32Checksum : public ChecksumVerifier {
public:
    Crc32Checksum() noexcept = default;
    ~Crc32Checksum() override = default;

    Crc32Checksum(const Crc32Checksum &) = delete;
    Crc32Checksum &operator=(const Crc32Checksum &) = delete;
    Crc32Checksum(Crc32Checksum &&other) noexcept;
    Crc32Checksum &operator=(Crc32Checksum &&other) noexcept;

    using ChecksumVerifier::update;

    void reset() override;
    void update(const void *buf, size_t len) override;
    std::string hex_digest() override;

    uint32_t crc32_value() const noexcept { return crc_; }

private:
    uint32_t crc_{0};
    bool finalized_{false};
    std::string cached_digest_;
};

} // namespace kale

#endif // KALE_CORE_CRC32_CHECKSUM_H
