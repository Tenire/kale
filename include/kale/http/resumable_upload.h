#ifndef KALE_RESUMABLE_UPLOAD_H
#define KALE_RESUMABLE_UPLOAD_H

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coke/task.h"
#include "kale/core/checksum.h"

namespace kale {


struct UploadSessionInfo {
    std::string upload_id;
    std::string target_path;
    uint64_t total_size{0};
    size_t part_size{0};
    uint32_t total_parts{0};
    std::vector<uint32_t> completed_parts;
    bool is_completed{false};
};
/**
 * @brief In-memory coordinator for chunked, resumable file uploads.
 *
 * Must outlive all active upload tasks. Coroutines return 0 on success, -ENOMEM on
 * allocation failure, -EIO on internal/verifier errors, or negative errno codes;
 * synchronous task frame construction may throw std::bad_alloc.
 */
class ResumableUploadCoordinator {
public:
    ResumableUploadCoordinator();
    ~ResumableUploadCoordinator();

    ResumableUploadCoordinator(const ResumableUploadCoordinator &) = delete;
    ResumableUploadCoordinator &operator=(const ResumableUploadCoordinator &) = delete;
    ResumableUploadCoordinator(ResumableUploadCoordinator &&) = delete;
    ResumableUploadCoordinator &operator=(ResumableUploadCoordinator &&) = delete;

    /**
     * @brief Initializes a chunked upload session and preallocates target storage.
     * Path reservations perform lexical comparisons without resolving symlinks.
     */
    coke::Task<int> init_session(
        std::string upload_id,
        std::string target_path,
        uint64_t total_size,
        size_t part_size
    );

    /**
     * @brief Writes a chunk part to its designated offset.
     *
     * Intermediate parts must match part_size exactly; the final part holds remainder bytes.
     * data and verifier are borrowed until task completion. Verifies expected_checksum before
     * writing (-EBADMSG on mismatch).
     */
    coke::Task<int> write_part(
        std::string upload_id,
        uint32_t part_number,
        std::string_view data,
        std::string expected_checksum = "",
        ChecksumVerifier *verifier = nullptr
    );

    std::optional<UploadSessionInfo> get_session(const std::string &upload_id) const;

    /**
     * @brief Finalizes an assembled upload session.
     *
     * Requires all parts completed and no writes in flight (-EBUSY otherwise).
     * Borrowed final_checksum computes digest across assembled content without validation.
     */
    coke::Task<int> complete_session(
        std::string upload_id,
        ChecksumVerifier *final_checksum = nullptr
    );

    /**
     * @brief Aborts an upload session and removes partial target files (-EBUSY if writes are active).
     */
    coke::Task<int> abort_session(std::string upload_id);

    /**
     * @brief Closes idle upload sessions (-EBUSY if writes are active).
     */
    coke::Task<int> clear();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kale

#endif // KALE_RESUMABLE_UPLOAD_H
