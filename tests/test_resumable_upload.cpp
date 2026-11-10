#include "doctest/doctest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "coke/future.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "kale/core/crc32_checksum.h"
#include "kale/security/evp_checksum.h"
#include "kale/http/resumable_upload.h"
namespace {

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix = "kale_resumable_test_") {
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
    std::string file_path(std::string_view filename) const {
        return (path_ / filename).string();
    }

    static std::string read_file(const std::string &filepath) {
        std::ifstream ifs(filepath, std::ios::binary);
        if (!ifs) return {};
        return std::string(std::istreambuf_iterator<char>(ifs),
                           std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path path_;
};

// Simple standard FNV-1a test verifier
class SimpleTestVerifier : public kale::ChecksumVerifier {
public:
    using ChecksumVerifier::update;
    SimpleTestVerifier() { reset(); }

    void reset() override {
        state_ = 0x811c9dc5u;
        finalized_ = false;
        cached_digest_.clear();
    }

    void update(const void *buf, size_t len) override {
        if (len == 0) return;
        if (!buf) {
            throw std::invalid_argument("null buffer with non-zero length");
        }
        if (finalized_) {
            throw std::logic_error("update after finalization");
        }
        const auto *p = static_cast<const uint8_t *>(buf);
        for (size_t i = 0; i < len; ++i) {
            state_ = (state_ ^ p[i]) * 0x01000193u;
        }
    }

    std::string hex_digest() override {
        if (!finalized_) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x", state_);
            cached_digest_ = std::string(buf);
            finalized_ = true;
        }
        return cached_digest_;
    }

private:
    uint32_t state_{0x811c9dc5u};
    bool finalized_{false};
    std::string cached_digest_;
};

// Throwing test verifiers to verify exception mapping and accounting restoration
class ThrowingTestVerifier : public kale::ChecksumVerifier {
public:
    using ChecksumVerifier::update;
    void reset() override {}
    void update(const void *, size_t) override {
        throw std::runtime_error("simulated_digest_failure");
    }
    std::string hex_digest() override {
        return "unused";
    }
};

class BadAllocTestVerifier : public kale::ChecksumVerifier {
public:
    using ChecksumVerifier::update;
    void reset() override {}
    void update(const void *, size_t) override {
        throw std::bad_alloc();
    }
    std::string hex_digest() override {
        return "unused";
    }
};

// Deterministic test verifier that coordinates active lifecycle state without production hooks
class BlockingTestVerifier : public kale::ChecksumVerifier {
public:
    using ChecksumVerifier::update;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};

    void reset() override {
        inner_.reset();
    }

    void update(const void *buf, size_t len) override {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        inner_.update(buf, len);
    }

    std::string hex_digest() override {
        return inner_.hex_digest();
    }

private:
    SimpleTestVerifier inner_;
};

class SimpleHttpClient {
public:
    explicit SimpleHttpClient(int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE_GE(fd_, 0);
        timeval timeout{5, 0};
        REQUIRE_EQ(::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        REQUIRE_EQ(::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)), 0);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE_EQ(::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    }

    ~SimpleHttpClient() {
        if (fd_ >= 0) ::close(fd_);
    }

    SimpleHttpClient(const SimpleHttpClient &) = delete;
    SimpleHttpClient &operator=(const SimpleHttpClient &) = delete;

    bool send_request(std::string_view method, std::string_view path,
                      std::string_view body = "",
                      std::string_view extra_headers = "") {
        std::string req = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\n"
                        + "Host: localhost\r\n"
                        + "Content-Length: " + std::to_string(body.size()) + "\r\n"
                        + "Connection: close\r\n"
                        + std::string(extra_headers) + "\r\n"
                        + std::string(body);
        size_t written = 0;
        while (written < req.size()) {
            ssize_t n = ::send(fd_, req.data() + written, req.size() - written, MSG_NOSIGNAL);
            if (n <= 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    }

    bool send_raw(std::string_view bytes) {
        size_t written = 0;
        while (written < bytes.size()) {
            ssize_t n = ::send(fd_, bytes.data() + written, bytes.size() - written, MSG_NOSIGNAL);
            if (n <= 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    }

    void close_socket() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::string read_response() {
        std::string resp;
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, static_cast<size_t>(n));
        }
        return resp;
    }

private:
    int fd_{-1};
};

} // namespace

TEST_SUITE("resumable_upload") {

TEST_CASE("ResumableUploadCoordinator: Out-of-order write and nonmultiple last-part length") {
    ScopedTempDir tmp;
    std::string target_path = tmp.file_path("out_of_order.bin");
    const std::string upload_id = "sess_ooo_1";

    const size_t part_size = 32 * 1024;
    const uint64_t total_size = 100 * 1024; // 32k + 32k + 32k + 4k (4 parts)
    const uint32_t total_parts = 4;

    std::string full_expected_data;
    full_expected_data.reserve(total_size);
    for (size_t i = 0; i < total_size; ++i) {
        full_expected_data.push_back(static_cast<char>('A' + (i % 26)));
    }

    kale::ResumableUploadCoordinator coordinator;
    int init_res = coke::sync_wait(coordinator.init_session(upload_id, target_path, total_size, part_size));
    REQUIRE_EQ(init_res, 0);

    auto sess_opt = coordinator.get_session(upload_id);
    REQUIRE(sess_opt.has_value());
    CHECK_EQ(sess_opt->total_parts, total_parts);
    CHECK_EQ(sess_opt->part_size, part_size);
    CHECK_EQ(sess_opt->total_size, total_size);
    CHECK_FALSE(sess_opt->is_completed);

    std::string part1 = full_expected_data.substr(0, 32 * 1024);
    std::string part2 = full_expected_data.substr(32 * 1024, 32 * 1024);
    std::string part3 = full_expected_data.substr(64 * 1024, 32 * 1024);
    std::string part4 = full_expected_data.substr(96 * 1024, 4 * 1024);

    // Out-of-order arrival: 3 -> 1 -> 4 -> 2
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 3, part3)), 0);
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 1, part1)), 0);
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 4, part4)), 0);

    sess_opt = coordinator.get_session(upload_id);
    REQUIRE(sess_opt.has_value());
    CHECK_FALSE(sess_opt->is_completed);
    CHECK_EQ(sess_opt->completed_parts.size(), 3);
    // Completed parts list is built on get_session and is sorted
    std::vector<uint32_t> expected_progress = {1, 3, 4};
    CHECK_EQ(sess_opt->completed_parts, expected_progress);

    // Write final missing part
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 2, part2)), 0);

    sess_opt = coordinator.get_session(upload_id);
    REQUIRE(sess_opt.has_value());
    CHECK(sess_opt->is_completed);
    CHECK_EQ(sess_opt->completed_parts.size(), 4);

    // Complete session
    SimpleTestVerifier final_verifier;
    CHECK_EQ(coke::sync_wait(coordinator.complete_session(upload_id, &final_verifier)), 0);

    // Verify exact bytes and size on disk
    CHECK(std::filesystem::exists(target_path));
    CHECK_EQ(std::filesystem::file_size(target_path), total_size);
    CHECK_EQ(ScopedTempDir::read_file(target_path), full_expected_data);

    // Verify final checksum matches expected digest
    SimpleTestVerifier expected_verifier;
    expected_verifier.update(full_expected_data);
    CHECK_EQ(final_verifier.hex_digest(), expected_verifier.hex_digest());

    // Completed session is removed
    CHECK_FALSE(coordinator.get_session(upload_id).has_value());
}

TEST_CASE("ResumableUploadCoordinator: Part length validation and failed-part retry") {
    ScopedTempDir tmp;
    std::string target_path = tmp.file_path("part_lengths.bin");
    const std::string upload_id = "sess_lengths_1";

    const size_t part_size = 16 * 1024;
    const uint64_t total_size = 40 * 1024; // 16k + 16k + 8k (3 parts)

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session(upload_id, target_path, total_size, part_size)), 0);

    std::string short_part(10 * 1024, 'X');
    std::string long_part(20 * 1024, 'Y');
    std::string exact_part(16 * 1024, 'A');
    std::string exact_last_part(8 * 1024, 'B');

    // Intermediate part length mismatch
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 1, short_part)), -EINVAL);
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 1, long_part)), -EINVAL);

    // Failed part is not marked complete and can be retried with exact length
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 1, exact_part)), 0);

    // Final part length mismatch
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 3, short_part)), -EINVAL);
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 3, exact_last_part)), 0);

    // Duplicate completed part returns -EEXIST
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 1, exact_part)), -EEXIST);

    // Part 2 still missing: complete returns -EBUSY
    CHECK_EQ(coke::sync_wait(coordinator.complete_session(upload_id)), -EBUSY);

    // Complete session via abort
    CHECK_EQ(coke::sync_wait(coordinator.abort_session(upload_id)), 0);
    CHECK_FALSE(std::filesystem::exists(target_path));
}

TEST_CASE("ResumableUploadCoordinator: Per-part checksum mismatch preserves data and state") {
    ScopedTempDir tmp;
    std::string target_path = tmp.file_path("checksum_test.bin");
    const std::string upload_id = "sess_chk_1";

    const size_t part_size = 16 * 1024;
    const uint64_t total_size = 32 * 1024;

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session(upload_id, target_path, total_size, part_size)), 0);

    std::string valid_part(16 * 1024, 'Z');
    SimpleTestVerifier verifier;
    verifier.update(valid_part);
    std::string correct_digest = verifier.hex_digest();

    // Checksum mismatch before mutation
    SimpleTestVerifier test_v1;
    int mismatch_res = coke::sync_wait(coordinator.write_part(
        upload_id, 1, valid_part, "00000000_bad_hash", &test_v1
    ));
    CHECK_EQ(mismatch_res, -EBADMSG);

    // Progress unchanged
    auto sess_opt = coordinator.get_session(upload_id);
    REQUIRE(sess_opt.has_value());
    CHECK(sess_opt->completed_parts.empty());

    // Retry with valid checksum succeeds
    SimpleTestVerifier test_v2;
    int valid_res = coke::sync_wait(coordinator.write_part(
        upload_id, 1, valid_part, correct_digest, &test_v2
    ));
    CHECK_EQ(valid_res, 0);

    sess_opt = coordinator.get_session(upload_id);
    REQUIRE(sess_opt.has_value());
    CHECK_EQ(sess_opt->completed_parts.size(), 1);

    CHECK_EQ(coke::sync_wait(coordinator.abort_session(upload_id)), 0);
}

TEST_CASE("ResumableUploadCoordinator: Verifier failure maps to negative error and restores accounting") {
    ScopedTempDir tmp;
    std::string path = tmp.file_path("verifier_ex.bin");
    const std::string upload_id = "sess_ver_ex";

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session(upload_id, path, 32 * 1024, 16 * 1024)), 0);

    std::string part_data(16 * 1024, 'E');

    // 1. Verifier throwing std::bad_alloc maps to -ENOMEM
    BadAllocTestVerifier bad_alloc_v;
    CHECK_EQ(coke::sync_wait(coordinator.write_part(
        upload_id, 1, part_data, "expected_hash", &bad_alloc_v
    )), -ENOMEM);

    // 2. Verifier throwing generic exception maps to -EIO (distinguished from EBADMSG)
    ThrowingTestVerifier throwing_v;
    CHECK_EQ(coke::sync_wait(coordinator.write_part(
        upload_id, 1, part_data, "expected_hash", &throwing_v
    )), -EIO);

    // Accounting was restored by RAII guard: session is not stuck busy, retry succeeds
    SimpleTestVerifier normal_v;
    normal_v.update(part_data);
    CHECK_EQ(coke::sync_wait(coordinator.write_part(
        upload_id, 1, part_data, normal_v.hex_digest(), &normal_v
    )), 0);

    CHECK_EQ(coke::sync_wait(coordinator.abort_session(upload_id)), 0);
}

TEST_CASE("ResumableUploadCoordinator: Duplicate session ID and lexical target path reservation") {
    ScopedTempDir tmp;
    std::string path1 = tmp.file_path("reserved1.bin");
    std::string path2 = tmp.file_path("reserved2.bin");

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session("sess_alpha", path1, 1024, 512)), 0);

    // Duplicate upload_id with different target path returns -EEXIST immediately
    CHECK_EQ(coke::sync_wait(coordinator.init_session("sess_alpha", path2, 1024, 512)), -EEXIST);

    // Distinct upload_id targeting same target_path returns -EEXIST
    CHECK_EQ(coke::sync_wait(coordinator.init_session("sess_beta", path1, 1024, 512)), -EEXIST);

    // Lexical path alias (e.g. ./dir/../path1)
    std::string lexical_alias = (tmp.path() / "sub" / ".." / "reserved1.bin").string();
    CHECK_EQ(coke::sync_wait(coordinator.init_session("sess_gamma", lexical_alias, 1024, 512)), -EEXIST);

    // After aborting sess_alpha, path1 reservation is released
    CHECK_EQ(coke::sync_wait(coordinator.abort_session("sess_alpha")), 0);
    CHECK_EQ(coke::sync_wait(coordinator.init_session("sess_beta", path1, 1024, 512)), 0);
    CHECK_EQ(coke::sync_wait(coordinator.abort_session("sess_beta")), 0);
}

TEST_CASE("ResumableUploadCoordinator: Malformed sizes and overflow validations") {
    ScopedTempDir tmp;
    std::string valid_path = tmp.file_path("valid.bin");

    kale::ResumableUploadCoordinator coordinator;

    // Zero sizes
    CHECK_EQ(coke::sync_wait(coordinator.init_session("s1", valid_path, 0, 1024)), -EINVAL);
    CHECK_EQ(coke::sync_wait(coordinator.init_session("s2", valid_path, 1024, 0)), -EINVAL);

    // Embedded NUL
    CHECK_EQ(coke::sync_wait(coordinator.init_session(std::string("s\0x", 3), valid_path, 1024, 512)), -EINVAL);
    CHECK_EQ(coke::sync_wait(coordinator.init_session("s3", std::string("/tmp/t\0x", 8), 1024, 512)), -EINVAL);

    // total_size > off_t max
    uint64_t overflow_size = static_cast<uint64_t>(std::numeric_limits<off_t>::max()) + 1ULL;
    CHECK_EQ(coke::sync_wait(coordinator.init_session("s4", valid_path, overflow_size, 1024)), -EFBIG);
}

TEST_CASE("ResumableUploadCoordinator: Failed init setup preserves existing file and releases reservations") {
    ScopedTempDir tmp;
    std::string regular_file = tmp.file_path("not_a_dir.txt");

    // Create a pre-existing regular file
    {
        std::ofstream ofs(regular_file, std::ios::binary);
        ofs << "PREEXISTING_REGULAR_FILE_CONTENT";
    }
    REQUIRE(std::filesystem::exists(regular_file));

    kale::ResumableUploadCoordinator coordinator;

    // Attempting to create a file inside a regular file parent fails with ENOTDIR on directory setup
    std::string invalid_nested_path = (tmp.path() / "not_a_dir.txt" / "child_file.bin").string();
    int err = coke::sync_wait(coordinator.init_session("fail_setup_id", invalid_nested_path, 1024, 512));
    CHECK_LT(err, 0);

    // Verify pre-existing regular file was NOT unlinked or modified
    CHECK(std::filesystem::exists(regular_file));
    CHECK_EQ(ScopedTempDir::read_file(regular_file), "PREEXISTING_REGULAR_FILE_CONTENT");

    // Verify reservation was cleanly rolled back: same upload_id is immediately reusable at a valid path
    std::string valid_file = tmp.file_path("valid_after_setup_fail.bin");
    CHECK_EQ(coke::sync_wait(coordinator.init_session("fail_setup_id", valid_file, 1024, 512)), 0);
    CHECK_EQ(coke::sync_wait(coordinator.abort_session("fail_setup_id")), 0);
}

TEST_CASE("ResumableUploadCoordinator: Real EvpChecksum and Crc32Checksum integration") {
    ScopedTempDir tmp;
    std::string target_path = tmp.file_path("concrete_checksums.bin");
    const std::string upload_id = "sess_concrete_chk";

    const size_t part_size = 16 * 1024;
    const uint64_t total_size = 32 * 1024; // 2 parts

    std::string full_data(total_size, '\0');
    for (size_t i = 0; i < total_size; ++i) {
        full_data[i] = static_cast<char>('a' + (i % 26));
    }

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session(upload_id, target_path, total_size, part_size)), 0);

    std::string part1 = full_data.substr(0, 16 * 1024);
    std::string part2 = full_data.substr(16 * 1024, 16 * 1024);

    // Compute actual CRC32 for part 1 using Crc32Checksum
    kale::Crc32Checksum crc_calc;
    crc_calc.update(part1);
    std::string part1_crc = crc_calc.hex_digest();

    // Write part 1 with Crc32Checksum
    kale::Crc32Checksum part1_verifier;
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 1, part1, part1_crc, &part1_verifier)), 0);

    // Compute actual SHA256 for part 2 using EvpChecksum
    kale::EvpChecksum sha_calc("SHA256");
    sha_calc.update(part2);
    std::string part2_sha = sha_calc.hex_digest();

    // Write part 2 with EvpChecksum
    kale::EvpChecksum part2_verifier("SHA256");
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 2, part2, part2_sha, &part2_verifier)), 0);

    // Complete session with EvpChecksum for full-file SHA256 digest
    kale::EvpChecksum full_calc("SHA256");
    full_calc.update(full_data);
    std::string expected_full_sha = full_calc.hex_digest();

    kale::EvpChecksum final_verifier("SHA256");
    CHECK_EQ(coke::sync_wait(coordinator.complete_session(upload_id, &final_verifier)), 0);
    CHECK_EQ(final_verifier.hex_digest(), expected_full_sha);

    CHECK_EQ(ScopedTempDir::read_file(target_path), full_data);
}

TEST_CASE("ResumableUploadCoordinator: clear rejects active sessions and cleans up idle sessions") {
    ScopedTempDir tmp;
    std::string path1 = tmp.file_path("clear1.bin");
    std::string path2 = tmp.file_path("clear2.bin");

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session("s_idle", path1, 1024, 512)), 0);
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session("s_busy", path2, 1024, 512)), 0);

    // Launch blocking write on s_busy
    BlockingTestVerifier blocking_verifier;
    std::string part_data(512, 'X');
    struct ReleaseGuard {
        BlockingTestVerifier &v;
        ~ReleaseGuard() { v.release.store(true, std::memory_order_release); }
    } guard{blocking_verifier};

    std::thread bg_thread([&]() {
        coke::sync_wait(coordinator.write_part("s_busy", 1, part_data, "bad", &blocking_verifier));
    });

    while (!blocking_verifier.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // clear() returns -EBUSY while active write is running
    CHECK_EQ(coke::sync_wait(coordinator.clear()), -EBUSY);

    blocking_verifier.release.store(true, std::memory_order_release);
    bg_thread.join();

    // Now all sessions are idle; clear() succeeds
    CHECK_EQ(coke::sync_wait(coordinator.clear()), 0);

    // Both sessions erased
    CHECK_FALSE(coordinator.get_session("s_idle").has_value());
    CHECK_FALSE(coordinator.get_session("s_busy").has_value());

    // Target paths can be re-reserved now
    CHECK_EQ(coke::sync_wait(coordinator.init_session("s_new", path1, 1024, 512)), 0);
    CHECK_EQ(coke::sync_wait(coordinator.abort_session("s_new")), 0);
}

TEST_CASE("ResumableUploadCoordinator: Deterministic concurrency admission via BlockingTestVerifier") {
    // Disclose: This tests lifecycle admission during verification, not overlapping kernel disk pwrite submissions
    ScopedTempDir tmp;
    std::string path = tmp.file_path("concurrency_admission.bin");
    const std::string upload_id = "sess_concurrent";

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session(upload_id, path, 64 * 1024, 32 * 1024)), 0);

    BlockingTestVerifier blocking_verifier;
    std::string part_data(32 * 1024, 'C');

    // RAII guard guarantees release on any failure
    struct ReleaseGuard {
        BlockingTestVerifier &v;
        ~ReleaseGuard() { v.release.store(true, std::memory_order_release); }
    } guard{blocking_verifier};

    // Launch in-flight write_part on a background thread
    std::atomic<int> bg_result{-999};
    std::thread bg_thread([&]() {
        bg_result.store(coke::sync_wait(coordinator.write_part(
            upload_id, 1, part_data, "any_checksum", &blocking_verifier
        )), std::memory_order_release);
    });

    // Wait until background task enters verifier (active accounting is active)
    while (!blocking_verifier.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // While write_part is active:
    // 1. Same-part write collision returns -EBUSY
    SimpleTestVerifier v2;
    CHECK_EQ(coke::sync_wait(coordinator.write_part(upload_id, 1, part_data, "", &v2)), -EBUSY);

    // 2. complete_session returns -EBUSY
    CHECK_EQ(coke::sync_wait(coordinator.complete_session(upload_id)), -EBUSY);

    // 3. abort_session returns -EBUSY
    CHECK_EQ(coke::sync_wait(coordinator.abort_session(upload_id)), -EBUSY);

    // Release blocking verifier to let background write proceed
    blocking_verifier.release.store(true, std::memory_order_release);
    bg_thread.join();

    // Background write returned; now session is quiescent and can be cleanly aborted
    CHECK_EQ(coke::sync_wait(coordinator.abort_session(upload_id)), 0);
}

TEST_CASE("ResumableUploadCoordinator: Real HTTP bounded multi-request resumable upload") {
    ScopedTempDir tmp;
    std::string out_file = tmp.file_path("http_uploaded.bin");
    const std::string upload_id = "http_upload_42";

    const size_t part_size = 16 * 1024;
    const uint64_t total_size = 40 * 1024; // 16k + 16k + 8k

    std::string full_expected_data;
    full_expected_data.reserve(total_size);
    for (size_t i = 0; i < total_size; ++i) {
        full_expected_data.push_back(static_cast<char>('0' + (i % 10)));
    }

    // Configure finite request_size_limit (32 KiB) to bound ingress buffering
    kale::HttpServerParams params;
    params.set_request_size_limit(32 * 1024);
    kale::HttpServer server(params);

    kale::ResumableUploadCoordinator coordinator;
    REQUIRE_EQ(coke::sync_wait(coordinator.init_session(upload_id, out_file, total_size, part_size)), 0);

    std::atomic<int> handler_entries{0};

    // Setup HTTP endpoints for chunked resumable upload
    server.PUT("/upload/:id/part/:num", [&coordinator, &handler_entries](kale::Context &ctx) -> coke::Task<void> {
        handler_entries.fetch_add(1, std::memory_order_relaxed);
        std::string id = std::string(ctx.param("id"));
        std::string num_str = std::string(ctx.param("num"));
        uint32_t num = static_cast<uint32_t>(std::stoul(num_str));

        int ret = co_await coordinator.write_part(id, num, ctx.req_body());
        if (ret == 0) {
            ctx.set_status(kale::HttpStatus::OK).string("part written");
        } else if (ret == -EBUSY) {
            ctx.set_status(kale::HttpStatus::Conflict).string("busy");
        } else if (ret == -EEXIST) {
            ctx.set_status(kale::HttpStatus::Conflict).string("duplicate");
        } else {
            ctx.set_status(kale::HttpStatus::BadRequest).string("error");
        }
    });

    server.POST("/upload/:id/complete", [&coordinator](kale::Context &ctx) -> coke::Task<void> {
        std::string id = std::string(ctx.param("id"));
        int ret = co_await coordinator.complete_session(id);
        if (ret == 0) {
            ctx.set_status(kale::HttpStatus::OK).string("completed");
        } else {
            ctx.set_status(kale::HttpStatus::BadRequest).string("incomplete");
        }
    });

    REQUIRE_EQ(server.start("127.0.0.1", 0), 0);
    int port = server.listen_port();

    // 1. Ingress bounding check: Request exceeding request_size_limit (48 KiB > 32 KiB)
    {
        SimpleHttpClient client(port);
        std::string oversized(48 * 1024, 'X');
        client.send_request("PUT", "/upload/" + upload_id + "/part/1", oversized);
        client.read_response();

        // Handler was NEVER dispatched for oversized request
        CHECK_EQ(handler_entries.load(std::memory_order_acquire), 0);
        // Session progress remains untouched
        auto snap = coordinator.get_session(upload_id);
        REQUIRE(snap.has_value());
        CHECK(snap->completed_parts.empty());
    }

    // 2. Partial declared part disconnect: send header Content-Length 16 KiB, send only 4 KiB then disconnect
    {
        SimpleHttpClient client(port);
        std::string req = "PUT /upload/" + upload_id + "/part/1 HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Content-Length: " + std::to_string(16 * 1024) + "\r\n"
                          "Connection: close\r\n\r\n"
                        + std::string(4 * 1024, 'P');
        client.send_raw(req);
        client.close_socket();
    }

    // Incomplete part was not dispatched or completed
    {
        auto snap = coordinator.get_session(upload_id);
        REQUIRE(snap.has_value());
        CHECK(snap->completed_parts.empty());
    }

    // 3. Multi-request resumable upload: Upload across distinct HTTP connections
    // Re-upload Part 1 (16 KiB) on a new connection
    {
        SimpleHttpClient client(port);
        std::string p1 = full_expected_data.substr(0, 16 * 1024);
        REQUIRE(client.send_request("PUT", "/upload/" + upload_id + "/part/1", p1));
        std::string resp = client.read_response();
        CHECK(resp.find("200 OK") != std::string::npos);
        CHECK_EQ(handler_entries.load(std::memory_order_acquire), 1);
    }

    // Connection interrupted / disconnected. Now resume on Connection 2: Upload part 2 (16 KiB)
    {
        SimpleHttpClient client(port);
        std::string p2 = full_expected_data.substr(16 * 1024, 16 * 1024);
        REQUIRE(client.send_request("PUT", "/upload/" + upload_id + "/part/2", p2));
        std::string resp = client.read_response();
        CHECK(resp.find("200 OK") != std::string::npos);
    }

    // Resume on Connection 3: Upload part 3 (8 KiB, final part)
    {
        SimpleHttpClient client(port);
        std::string p3 = full_expected_data.substr(32 * 1024, 8 * 1024);
        REQUIRE(client.send_request("PUT", "/upload/" + upload_id + "/part/3", p3));
        std::string resp = client.read_response();
        CHECK(resp.find("200 OK") != std::string::npos);
    }

    // Connection 4: Complete session
    {
        SimpleHttpClient client(port);
        REQUIRE(client.send_request("POST", "/upload/" + upload_id + "/complete"));
        std::string resp = client.read_response();
        CHECK(resp.find("200 OK") != std::string::npos);
    }

    server.stop();

    // Verify exact file on disk
    CHECK(std::filesystem::exists(out_file));
    CHECK_EQ(std::filesystem::file_size(out_file), total_size);
    CHECK_EQ(ScopedTempDir::read_file(out_file), full_expected_data);
}

} // TEST_SUITE("resumable_upload")
