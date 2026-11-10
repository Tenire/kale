#include "doctest/doctest.h"
#include "kale/core/crc32_checksum.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
TEST_SUITE("crc32_checksum") {

TEST_CASE("Empty input digest and crc32_value") {
    kale::Crc32Checksum verifier;
    CHECK(verifier.crc32_value() == 0);
    CHECK(verifier.hex_digest() == "00000000");
    CHECK(verifier.crc32_value() == 0);

    // Finalized result is cached
    CHECK(verifier.hex_digest() == "00000000");

    // Zero-length update after reset is a no-op
    verifier.reset();
    verifier.update("", 0);
    verifier.update(nullptr, 0);
    CHECK(verifier.hex_digest() == "00000000");
}

TEST_CASE("Standard CRC32 test vectors and exact lowercase 8-digit hex") {
    struct VectorEntry {
        std::string_view input;
        std::string_view expected_hex;
        uint32_t expected_val;
    };

    const VectorEntry vectors[] = {
        {"", "00000000", 0x00000000U},
        {"a", "e8b7be43", 0xe8b7be43U},
        {"abc", "352441c2", 0x352441c2U},
        {"message digest", "20159d7f", 0x20159d7fU},
        {"123456789", "cbf43926", 0xcbf43926U},
        {"The quick brown fox jumps over the lazy dog", "414fa339", 0x414fa339U},
        {"test_10", "003946e8", 0x003946e8U}, // Verifies zero-padding with two leading zeros
    };

    for (const auto &entry : vectors) {
        // Test raw pointer + size update
        kale::Crc32Checksum v1;
        v1.update(entry.input.data(), entry.input.size());
        CHECK(v1.crc32_value() == entry.expected_val);
        std::string hex1 = v1.hex_digest();
        CHECK(hex1 == entry.expected_hex);
        CHECK(hex1.size() == 8);

        // Test string_view update overload
        kale::Crc32Checksum v2;
        v2.update(entry.input);
        CHECK(v2.crc32_value() == entry.expected_val);
        std::string hex2 = v2.hex_digest();
        CHECK(hex2 == entry.expected_hex);
        CHECK(hex2.size() == 8);
    }
}

TEST_CASE("Incremental chunked input vs single update") {
    // Arbitrary non-trivial chunk boundaries
    kale::Crc32Checksum chunked;
    chunked.update("12");
    chunked.update("345");
    chunked.update(""); // zero-length no-op
    chunked.update("6");
    chunked.update("789");

    CHECK(chunked.crc32_value() == 0xcbf43926U);
    CHECK(chunked.hex_digest() == "cbf43926");

    // Single-byte feeding
    const std::string_view fox = "The quick brown fox jumps over the lazy dog";
    kale::Crc32Checksum byte_by_byte;
    for (char c : fox) {
        byte_by_byte.update(&c, 1);
    }
    CHECK(byte_by_byte.crc32_value() == 0x414fa339U);
    CHECK(byte_by_byte.hex_digest() == "414fa339");

    // Bounded multi-KB chunking comparison without giant allocations
    std::string payload;
    payload.reserve(64 * 1024);
    for (size_t i = 0; i < 64 * 1024; ++i) {
        payload.push_back(static_cast<char>('A' + (i % 26)));
    }

    kale::Crc32Checksum whole;
    whole.update(payload);
    std::string whole_hex = whole.hex_digest();

    kale::Crc32Checksum sliced;
    size_t offset = 0;
    const size_t step = 3072;
    while (offset < payload.size()) {
        size_t n = std::min(step, payload.size() - offset);
        sliced.update(payload.data() + offset, n);
        offset += n;
    }
    CHECK(sliced.hex_digest() == whole_hex);
    CHECK(sliced.crc32_value() == whole.crc32_value());
}

TEST_CASE("Null buffer rejection with positive length") {
    kale::Crc32Checksum verifier;

    // Positive length with nullptr must throw std::invalid_argument
    CHECK_THROWS_AS(verifier.update(nullptr, 1), std::invalid_argument);
    CHECK_THROWS_AS(verifier.update(nullptr, 1024), std::invalid_argument);

    // Zero length with nullptr must not throw and must be a no-op
    CHECK_NOTHROW(verifier.update(nullptr, 0));

    // Internal state remains healthy after rejected calls
    verifier.update("123456789");
    CHECK(verifier.crc32_value() == 0xcbf43926U);
    CHECK(verifier.hex_digest() == "cbf43926");
}

TEST_CASE("Finalization caching and update misuse rejection") {
    kale::Crc32Checksum verifier;
    verifier.update("123456789");

    std::string digest = verifier.hex_digest();
    CHECK(digest == "cbf43926");

    // Repeated hex_digest calls return the same cached value
    CHECK(verifier.hex_digest() == "cbf43926");

    // Zero-length update after finalization is a no-op and preserves the cached digest
    CHECK_NOTHROW(verifier.update("", 0));
    CHECK_NOTHROW(verifier.update(nullptr, 0));
    CHECK_NOTHROW(verifier.update(std::string_view("")));
    CHECK(verifier.hex_digest() == "cbf43926");
    CHECK(verifier.crc32_value() == 0xcbf43926U);

    // Positive-length update after finalization throws std::logic_error
    CHECK_THROWS_AS(verifier.update("extra"), std::logic_error);
    CHECK_THROWS_AS(verifier.update(nullptr, 1), std::logic_error);
    CHECK(verifier.hex_digest() == "cbf43926");
    // Reset clears finalization state and accumulator
    verifier.reset();
    CHECK(verifier.crc32_value() == 0);
    verifier.update("abc");
    CHECK(verifier.crc32_value() == 0x352441c2U);
    CHECK(verifier.hex_digest() == "352441c2");

    // Multiple consecutive resets are safe
    verifier.reset();
    verifier.reset();
    CHECK(verifier.crc32_value() == 0);
    CHECK(verifier.hex_digest() == "00000000");
}

TEST_CASE("Move construction and move assignment") {
    kale::Crc32Checksum v1;
    v1.update("123");

    kale::Crc32Checksum v2(std::move(v1));
    v2.update("456789");
    CHECK(v2.crc32_value() == 0xcbf43926U);
    CHECK(v2.hex_digest() == "cbf43926");

    kale::Crc32Checksum v3;
    v3 = std::move(v2);
    CHECK(v3.crc32_value() == 0xcbf43926U);
    CHECK(v3.hex_digest() == "cbf43926");
}

TEST_CASE("Polymorphic usage through ChecksumVerifier base interface") {
    std::unique_ptr<kale::ChecksumVerifier> verifier = std::make_unique<kale::Crc32Checksum>();

    verifier->update(std::string_view("123456789"));
    CHECK(verifier->hex_digest() == "cbf43926");

    // Zero-length update after finalization is a no-op and preserves cached digest
    CHECK_NOTHROW(verifier->update(std::string_view("")));
    CHECK(verifier->hex_digest() == "cbf43926");

    // Positive-length rejection on base class interface as well
    CHECK_THROWS_AS(verifier->update("more"), std::logic_error);
    verifier->reset();
    verifier->update(std::string_view("abc"));
    CHECK(verifier->hex_digest() == "352441c2");
}

} // TEST_SUITE("crc32_checksum")
