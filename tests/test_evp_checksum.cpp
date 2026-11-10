#include "doctest/doctest.h"

#include "kale/security/evp_checksum.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

TEST_SUITE("evp_checksum") {

TEST_CASE("noncopyable type traits") {
    CHECK_FALSE(std::is_copy_constructible_v<kale::EvpChecksum>);
    CHECK_FALSE(std::is_copy_assignable_v<kale::EvpChecksum>);
    CHECK(std::is_move_constructible_v<kale::EvpChecksum>);
    CHECK(std::is_move_assignable_v<kale::EvpChecksum>);
}

TEST_CASE("empty input for standard algorithms produces standard empty digest") {
    // Default constructor uses SHA256
    {
        kale::EvpChecksum verifier;
        CHECK_EQ(verifier.algorithm_name(), "SHA256");
        CHECK_FALSE(verifier.is_finalized());
        // Standard SHA256("")
        CHECK_EQ(verifier.hex_digest(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        CHECK(verifier.is_finalized());
    }

    // Explicit MD5 empty input
    {
        kale::EvpChecksum verifier("MD5");
        CHECK_EQ(verifier.algorithm_name(), "MD5");
        CHECK_EQ(verifier.hex_digest(), "d41d8cd98f00b204e9800998ecf8427e");
    }

    // Explicit SHA512 empty input
    {
        kale::EvpChecksum verifier("SHA512");
        CHECK_EQ(verifier.algorithm_name(), "SHA512");
        CHECK_EQ(verifier.hex_digest(),
                 "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                 "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    }

    // Custom OpenSSL EVP algorithm: SHA384
    {
        kale::EvpChecksum verifier("SHA384");
        CHECK_EQ(verifier.hex_digest(),
                 "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
                 "274edebfe76f65fbd51ad2f14898b95b");
    }

    // Custom OpenSSL EVP algorithm: SHA1
    {
        kale::EvpChecksum verifier("SHA1");
        CHECK_EQ(verifier.hex_digest(), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    }

    // Zero-length updates with valid or null pointer do not change empty digest
    {
        kale::EvpChecksum verifier;
        verifier.update(nullptr, 0);
        const char dummy = 'x';
        verifier.update(&dummy, 0);
        verifier.update(std::string_view(""));
        CHECK_EQ(verifier.hex_digest(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }
}

TEST_CASE("known standard vectors for MD5, SHA256, SHA512, and custom EVP digests") {
    // Vectors for "abc"
    {
        kale::EvpChecksum md5("MD5");
        md5.update("abc");
        CHECK_EQ(md5.hex_digest(), "900150983cd24fb0d6963f7d28e17f72");

        kale::EvpChecksum sha256("SHA256");
        sha256.update("abc");
        CHECK_EQ(sha256.hex_digest(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        kale::EvpChecksum sha512("SHA512");
        sha512.update("abc");
        CHECK_EQ(sha512.hex_digest(),
                 "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                 "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

        kale::EvpChecksum sha384("SHA384");
        sha384.update("abc");
        CHECK_EQ(sha384.hex_digest(),
                 "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                 "8086072ba1e7cc2358baeca134c825a7");

        kale::EvpChecksum sha1("SHA1");
        sha1.update("abc");
        CHECK_EQ(sha1.hex_digest(), "a9993e364706816aba3e25717850c26c9cd0d89d");
    }

    // Vectors for "The quick brown fox jumps over the lazy dog"
    const std::string_view fox = "The quick brown fox jumps over the lazy dog";
    {
        kale::EvpChecksum md5("md5"); // lowercase name
        md5.update(fox);
        CHECK_EQ(md5.hex_digest(), "9e107d9d372bb6826bd81d3542a419d6");

        kale::EvpChecksum sha256("sha256"); // lowercase name
        sha256.update(fox);
        CHECK_EQ(sha256.hex_digest(), "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");

        kale::EvpChecksum sha512("sha512"); // lowercase name
        sha512.update(fox);
        CHECK_EQ(sha512.hex_digest(),
                 "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb64"
                 "2e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6");
    }
}

TEST_CASE("multi-update incremental feed matches single whole-input feed") {
    const std::string text = "Resumable uploads require byte-for-byte fidelity across out-of-order and chunked network transport.";

    // Compare whole-input vs varying chunk sizes for SHA-256
    {
        kale::EvpChecksum whole;
        whole.update(text);
        const std::string expected_sha256 = whole.hex_digest();

        // 1-byte chunks
        kale::EvpChecksum by_byte;
        for (char c : text) {
            by_byte.update(&c, 1);
        }
        CHECK_EQ(by_byte.hex_digest(), expected_sha256);

        // Arbitrary sliced chunks with zero-length chunks interleaved
        kale::EvpChecksum sliced;
        size_t offset = 0;
        const size_t step_sizes[] = {5, 0, 11, 3, 0, 17, 2, 23, 7, 0, 100};
        for (size_t step : step_sizes) {
            if (offset >= text.size()) break;
            size_t take = (offset + step > text.size()) ? (text.size() - offset) : step;
            sliced.update(text.data() + offset, take);
            offset += take;
        }
        CHECK_EQ(sliced.hex_digest(), expected_sha256);
    }

    // Compare whole-input vs multi-part updates for MD5
    {
        kale::EvpChecksum whole("MD5");
        whole.update(text);
        const std::string expected_md5 = whole.hex_digest();

        kale::EvpChecksum chunked("MD5");
        size_t mid = text.size() / 2;
        chunked.update(std::string_view(text.data(), mid));
        chunked.update(std::string_view(text.data() + mid, text.size() - mid));
        CHECK_EQ(chunked.hex_digest(), expected_md5);
    }
}

TEST_CASE("reset clears accumulator and cached digest, enabling full reuse") {
    kale::EvpChecksum verifier;

    // First calculation
    verifier.update("first message");
    std::string digest1 = verifier.hex_digest();
    CHECK(verifier.is_finalized());

    // Reset allows a completely new calculation
    verifier.reset();
    CHECK_FALSE(verifier.is_finalized());

    verifier.update("second message");
    std::string digest2 = verifier.hex_digest();
    CHECK(verifier.is_finalized());
    CHECK_NE(digest1, digest2);

    // Reset multiple times consecutively
    verifier.reset();
    verifier.reset();
    verifier.reset();
    CHECK_FALSE(verifier.is_finalized());

    // Resetting and finalizing immediately produces empty string digest
    CHECK_EQ(verifier.hex_digest(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // Reset before finalizing discards in-flight accumulator
    verifier.reset();
    verifier.update("aborted in-flight data");
    verifier.reset();
    verifier.update("abc");
    CHECK_EQ(verifier.hex_digest(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("finalization caching and misuse handling") {
    kale::EvpChecksum verifier;
    verifier.update("abc");
    std::string digest = verifier.hex_digest();
    CHECK_EQ(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(verifier.is_finalized());

    // Repeated hex_digest calls return cached digest
    CHECK_EQ(verifier.hex_digest(), digest);
    CHECK_EQ(verifier.hex_digest(), digest);

    // Cross-adapter contract: len==0 is a no-op even after successful finalization, preserving cached digest
    verifier.update(nullptr, 0);
    const char dummy = 'z';
    verifier.update(&dummy, 0);
    verifier.update(std::string_view(""));
    CHECK_EQ(verifier.hex_digest(), digest);

    // Positive-length update after finalization rejects until reset
    CHECK_THROWS_AS(verifier.update("more data"), std::logic_error);
    CHECK_THROWS_AS(verifier.update(&dummy, 1), std::logic_error);

    // Cached digest is still preserved after rejected update
    CHECK_EQ(verifier.hex_digest(), digest);

    // Reset permits updates again
    verifier.reset();
    CHECK_FALSE(verifier.is_finalized());
    verifier.update("def");
    CHECK_NE(verifier.hex_digest(), digest);
}

TEST_CASE("unsupported, empty, or embedded NUL algorithm names reject at construction") {
    // Empty algorithm name
    CHECK_THROWS_AS(kale::EvpChecksum(""), std::invalid_argument);

    // Embedded NUL characters must not be silently truncated by c_str
    CHECK_THROWS_AS(kale::EvpChecksum(std::string_view("SHA256\0extra", 12)), std::invalid_argument);
    CHECK_THROWS_AS(kale::EvpChecksum(std::string_view("MD5\0", 4)), std::invalid_argument);
    CHECK_THROWS_AS(kale::EvpChecksum(std::string_view("\0SHA256", 7)), std::invalid_argument);
    CHECK_THROWS_AS(kale::EvpChecksum(std::string_view("sha\0512", 7)), std::invalid_argument);

    // Unknown algorithm name
    CHECK_THROWS_AS(kale::EvpChecksum("non_existent_algorithm_xyz"), std::invalid_argument);
    CHECK_THROWS_AS(kale::EvpChecksum("BOGUS_SHA999"), std::invalid_argument);
}

TEST_CASE("null buffer with positive length rejects without state corruption") {
    kale::EvpChecksum verifier;
    verifier.update("prefix-");

    // Null buffer with positive length throws std::invalid_argument
    CHECK_THROWS_AS(verifier.update(nullptr, 1), std::invalid_argument);
    CHECK_THROWS_AS(verifier.update(nullptr, 1024), std::invalid_argument);

    // Accumulator state remains intact: adding remaining suffix yields correct sha256("prefix-suffix")
    verifier.update("suffix");
    kale::EvpChecksum reference;
    reference.update("prefix-suffix");
    CHECK_EQ(verifier.hex_digest(), reference.hex_digest());
}

TEST_CASE("ownership of algorithm name and move semantics") {
    // Caller temporary string disappears: internal string copy prevents dangling
    {
        std::string algo = "SHA256";
        kale::EvpChecksum verifier{std::string_view{algo}};
        algo.replace(0, algo.size(), "CORRUPTED");
        CHECK_EQ(verifier.algorithm_name(), "SHA256");
    }

    // Move constructor
    {
        kale::EvpChecksum source;
        source.update("abc");

        kale::EvpChecksum target(std::move(source));
        CHECK_EQ(target.algorithm_name(), "SHA256");
        CHECK_EQ(target.hex_digest(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }

    // Move assignment
    {
        kale::EvpChecksum source("MD5");
        source.update("abc");

        kale::EvpChecksum target("SHA256");
        target = std::move(source);
        CHECK_EQ(target.algorithm_name(), "MD5");
        CHECK_EQ(target.hex_digest(), "900150983cd24fb0d6963f7d28e17f72");
    }

    // Polymorphic ChecksumVerifier pointer dispatch
    {
        kale::EvpChecksum evp("SHA256");
        kale::ChecksumVerifier *verifier = &evp;
        verifier->update("abc");
        CHECK_EQ(verifier->hex_digest(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}

} // TEST_SUITE
