#include "kale/core/crc32_checksum.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

#include <zlib.h>

namespace kale {

Crc32Checksum::Crc32Checksum(Crc32Checksum &&other) noexcept
    : crc_(other.crc_),
      finalized_(other.finalized_),
      cached_digest_(std::move(other.cached_digest_)) {
    other.crc_ = 0;
    other.finalized_ = false;
    other.cached_digest_.clear();
}

Crc32Checksum &Crc32Checksum::operator=(Crc32Checksum &&other) noexcept {
    if (this != &other) {
        crc_ = other.crc_;
        finalized_ = other.finalized_;
        cached_digest_ = std::move(other.cached_digest_);
        other.crc_ = 0;
        other.finalized_ = false;
        other.cached_digest_.clear();
    }
    return *this;
}

void Crc32Checksum::reset() {
    crc_ = 0;
    finalized_ = false;
    cached_digest_.clear();
}

void Crc32Checksum::update(const void *buf, size_t len) {
    if (len == 0) {
        return;
    }
    if (finalized_) {
        throw std::logic_error("Crc32Checksum: update called after finalization without reset");
    }
    if (buf == nullptr) {
        throw std::invalid_argument("Crc32Checksum: buffer is null with non-zero length");
    }
    const auto *bytes = static_cast<const Bytef *>(buf);
    constexpr size_t kMaxChunk = static_cast<size_t>(std::numeric_limits<uInt>::max());
    while (len > 0) {
        const uInt chunk = static_cast<uInt>(std::min(len, kMaxChunk));
        crc_ = static_cast<uint32_t>(::crc32(static_cast<uLong>(crc_), bytes, chunk));
        bytes += chunk;
        len -= chunk;
    }
}

std::string Crc32Checksum::hex_digest() {
    if (finalized_) {
        return cached_digest_;
    }

    char buf[9];
    int written = std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned int>(crc_));
    if (written != 8) {
        throw std::runtime_error("Crc32Checksum: failed to format hex digest");
    }

    cached_digest_.assign(buf, 8);
    finalized_ = true;
    return cached_digest_;
}

} // namespace kale
