#ifndef KALE_CORE_CHECKSUM_H
#define KALE_CORE_CHECKSUM_H

#include <cstddef>
#include <string>
#include <string_view>

namespace kale {

/**
 * @brief Incremental checksum accumulator.
 *
 * Accumulators are stateful and not thread-safe. hex_digest() finalizes and
 * caches the lowercase hex digest until reset(). Zero-length updates are no-ops
 * even when finalized. For nonzero length, update() throws std::invalid_argument
 * if buf is null, and std::logic_error if called after finalization before reset().
 */
class ChecksumVerifier {
public:
    virtual ~ChecksumVerifier() = default;

    virtual void reset() = 0;
    virtual void update(const void *buf, size_t len) = 0;
    void update(std::string_view data) { update(data.data(), data.size()); }
    virtual std::string hex_digest() = 0;
};

} // namespace kale

#endif // KALE_CORE_CHECKSUM_H
