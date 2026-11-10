#include "doctest/doctest.h"

#include <string>
#include <string_view>
#include <vector>

#include "coke/http/http_client.h"
#include "coke/wait.h"
#include "kale/core/context.h"
#include "kale/core/http_server.h"
#include "workflow/HttpMessage.h"
#include "workflow/WFTaskFactory.h"

namespace {

inline std::string get_resp_body_merged(const kale::Context &ctx) {
    std::string out;
    ctx.get_resp_body_merged(out);
    return out;
}

} // namespace

TEST_SUITE("core") {

TEST_CASE("Response hotpath: initial body setting on fresh empty response") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/probe_body", [](kale::Context &ctx) {
        // Response starts empty: raw_resp().get_output_body_size() == 0
        CHECK_EQ(ctx.resp_body_size(), 0);

        static constexpr std::string_view kStaticPayload = "fastpath-zero-copy-native-body-payload";
        ctx.body(kStaticPayload);

        CHECK_EQ(ctx.resp_body_size(), kStaticPayload.size());
        CHECK_EQ(get_resp_body_merged(ctx), kStaticPayload);
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    auto *task = WFTaskFactory::create_http_task("http://127.0.0.1:" + std::to_string(port) + "/probe_body", 0, 1, nullptr);
    auto res = coke::sync_wait(coke::HttpAwaiter(task));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    CHECK_EQ(std::string_view(static_cast<const char*>(body_ptr), body_len), "fastpath-zero-copy-native-body-payload");

    server.stop();
}

TEST_CASE("Response hotpath: self-alias safety across exact, sub-slice, and overlapping replacements") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/test_slices", [](kale::Context &ctx) {
        // 1. Initial payload
        ctx.body("abcdefghijklmnopqrstuvwxyz_0123456789");
        CHECK_EQ(get_resp_body_merged(ctx), "abcdefghijklmnopqrstuvwxyz_0123456789");

        // Obtain block pointer to existing body
        const void *bufs[1];
        size_t sizes[1];
        size_t n = ctx.get_resp_body_blocks(bufs, sizes, 1);
        REQUIRE_EQ(n, 1);
        std::string_view full_slice(static_cast<const char*>(bufs[0]), sizes[0]);

        // 2. Exact self-alias replacement
        ctx.body(full_slice);
        CHECK_EQ(get_resp_body_merged(ctx), "abcdefghijklmnopqrstuvwxyz_0123456789");

        // Refresh block pointer
        n = ctx.get_resp_body_blocks(bufs, sizes, 1);
        REQUIRE_EQ(n, 1);
        full_slice = std::string_view(static_cast<const char*>(bufs[0]), sizes[0]);

        // 3. Prefix slice replacement
        std::string_view prefix = full_slice.substr(0, 6);
        ctx.body(prefix);
        CHECK_EQ(get_resp_body_merged(ctx), "abcdef");

        // 4. Middle slice replacement
        n = ctx.get_resp_body_blocks(bufs, sizes, 1);
        REQUIRE_EQ(n, 1);
        std::string_view mid_slice(static_cast<const char*>(bufs[0]), sizes[0]);
        std::string_view mid = mid_slice.substr(2, 3); // "cde"
        ctx.body(mid);
        CHECK_EQ(get_resp_body_merged(ctx), "cde");

        // 5. Suffix slice replacement
        n = ctx.get_resp_body_blocks(bufs, sizes, 1);
        REQUIRE_EQ(n, 1);
        std::string_view suf_slice(static_cast<const char*>(bufs[0]), sizes[0]);
        std::string_view suf = suf_slice.substr(1, 2); // "de"
        ctx.body(suf);
        CHECK_EQ(get_resp_body_merged(ctx), "de");

        // 6. Overlapping slice within multi-fragment body
        ctx.raw_resp().clear_output_body();
        ctx.raw_resp().append_output_body("hello_", 6);
        ctx.raw_resp().append_output_body("world_", 6);
        ctx.raw_resp().append_output_body("12345", 5);

        const void *frag_bufs[3];
        size_t frag_sizes[3];
        size_t nfrag = ctx.get_resp_body_blocks(frag_bufs, frag_sizes, 3);
        REQUIRE_EQ(nfrag, 3);

        // Alias from second block
        std::string_view b2(static_cast<const char*>(frag_bufs[1]), frag_sizes[1]);
        ctx.body(b2.substr(0, 5)); // "world"
        CHECK_EQ(get_resp_body_merged(ctx), "world");
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    auto *task = WFTaskFactory::create_http_task("http://127.0.0.1:" + std::to_string(port) + "/test_slices", 0, 1, nullptr);
    auto res = coke::sync_wait(coke::HttpAwaiter(task));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    CHECK_EQ(std::string_view(static_cast<const char*>(body_ptr), body_len), "world");

    server.stop();
}

TEST_CASE("Response hotpath: large output alias detection beyond 128 blocks with RAII growth") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/test_large_blocks", [](kale::Context &ctx) {
        // Construct response with 150 distinct blocks (> 128 blocks)
        constexpr size_t NUM_BLOCKS = 150;
        std::vector<std::string> block_storage;
        block_storage.reserve(NUM_BLOCKS);

        ctx.raw_resp().clear_output_body();
        for (size_t i = 0; i < NUM_BLOCKS; ++i) {
            block_storage.push_back("blk#" + std::to_string(i) + ";");
            ctx.raw_resp().append_output_body(block_storage.back().data(), block_storage.back().size());
        }

        CHECK_GE(ctx.resp_body_size(), NUM_BLOCKS * 6);

        // Retrieve blocks into large buffer
        std::vector<const void *> bufs(NUM_BLOCKS);
        std::vector<size_t> sizes(NUM_BLOCKS);
        size_t fetched = ctx.get_resp_body_blocks(bufs.data(), sizes.data(), NUM_BLOCKS);
        REQUIRE_EQ(fetched, NUM_BLOCKS);

        // 1. Alias slice from block #140 (strictly beyond the 128-block boundary)
        std::string_view slice_140(static_cast<const char*>(bufs[140]), sizes[140]);
        CHECK_EQ(slice_140, "blk#140;");

        // Replacing body with slice_140 must detect alias through dynamic RAII vector growth
        ctx.body(slice_140);
        CHECK_EQ(get_resp_body_merged(ctx), "blk#140;");

        // 2. Repopulate with 140 blocks and test slice from block #5
        ctx.raw_resp().clear_output_body();
        block_storage.clear();
        for (size_t i = 0; i < 140; ++i) {
            block_storage.push_back("item_" + std::to_string(i) + "|");
            ctx.raw_resp().append_output_body(block_storage.back().data(), block_storage.back().size());
        }

        fetched = ctx.get_resp_body_blocks(bufs.data(), sizes.data(), 140);
        REQUIRE_EQ(fetched, 140);

        std::string_view slice_5(static_cast<const char*>(bufs[5]), sizes[5]);
        CHECK_EQ(slice_5, "item_5|");

        ctx.body(slice_5);
        CHECK_EQ(get_resp_body_merged(ctx), "item_5|");
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    auto *task = WFTaskFactory::create_http_task("http://127.0.0.1:" + std::to_string(port) + "/test_large_blocks", 0, 1, nullptr);
    auto res = coke::sync_wait(coke::HttpAwaiter(task));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    CHECK_EQ(std::string_view(static_cast<const char*>(body_ptr), body_len), "item_5|");

    server.stop();
}
TEST_CASE("Response hotpath: 9-32 block non-alias replacement within fixed stack envelope") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/test_medium_blocks", [](kale::Context &ctx) {
        // Pre-populate response with 20 blocks (strictly between 9 and 32)
        constexpr size_t NUM_BLOCKS = 20;
        std::vector<std::string> blocks;
        blocks.reserve(NUM_BLOCKS);
        for (size_t i = 0; i < NUM_BLOCKS; ++i) {
            blocks.push_back("chunk_" + std::to_string(i) + ";");
            ctx.raw_resp().append_output_body(blocks.back().data(), blocks.back().size());
        }

        size_t expected_total_size = 0;
        for (const auto &b : blocks) {
            expected_total_size += b.size();
        }
        CHECK_EQ(ctx.resp_body_size(), expected_total_size);

        // Replace body with a completely independent non-aliasing string
        static constexpr std::string_view kFreshPayload = "completely-independent-new-payload";
        ctx.body(kFreshPayload);

        // Body must now contain only the fresh payload
        CHECK_EQ(ctx.resp_body_size(), kFreshPayload.size());
        CHECK_EQ(get_resp_body_merged(ctx), kFreshPayload);

        // Also test self-alias slice from within the 20-block envelope
        ctx.raw_resp().clear_output_body();
        for (size_t i = 0; i < NUM_BLOCKS; ++i) {
            ctx.raw_resp().append_output_body(blocks[i].data(), blocks[i].size());
        }

        const void *bufs[NUM_BLOCKS];
        size_t sizes[NUM_BLOCKS];
        size_t fetched = ctx.get_resp_body_blocks(bufs, sizes, NUM_BLOCKS);
        REQUIRE_EQ(fetched, NUM_BLOCKS);

        std::string_view slice_15(static_cast<const char*>(bufs[15]), sizes[15]);
        CHECK_EQ(slice_15, "chunk_15;");

        ctx.body(slice_15);
        CHECK_EQ(get_resp_body_merged(ctx), "chunk_15;");
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    auto *task = WFTaskFactory::create_http_task("http://127.0.0.1:" + std::to_string(port) + "/test_medium_blocks", 0, 1, nullptr);
    auto res = coke::sync_wait(coke::HttpAwaiter(task));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    CHECK_EQ(std::string_view(static_cast<const char*>(body_ptr), body_len), "chunk_15;");

    server.stop();
}

TEST_CASE("Response hotpath: repeated, empty, binary, and temporary replacements") {
    kale::HttpServerParams params;
    kale::HttpServer server(params);

    server.GET("/test_misc", [](kale::Context &ctx) {
        // Repeated replacements
        for (int i = 0; i < 10; ++i) {
            std::string payload = "iter_" + std::to_string(i);
            ctx.body(payload);
            CHECK_EQ(get_resp_body_merged(ctx), payload);
        }

        // Empty body clears body and size becomes 0
        ctx.body("");
        CHECK_EQ(ctx.resp_body_size(), 0);
        CHECK_EQ(get_resp_body_merged(ctx), "");

        // Binary body with null bytes
        const std::string binary_data(std::string_view("bin\0ary\0\xFF\x00data", 14));
        ctx.body(binary_data);
        CHECK_EQ(ctx.resp_body_size(), 14);
        CHECK_EQ(get_resp_body_merged(ctx), binary_data);

        // Self-alias of binary body
        const void *bufs[1];
        size_t sizes[1];
        size_t n = ctx.get_resp_body_blocks(bufs, sizes, 1);
        REQUIRE_EQ(n, 1);
        std::string_view bin_slice(static_cast<const char*>(bufs[0]), sizes[0]);
        ctx.body(bin_slice.substr(0, 7)); // "bin\0ary"
        CHECK_EQ(get_resp_body_merged(ctx), std::string("bin\0ary", 7));

        // Temporary rvalue string
        ctx.string(std::string("transient_string_12345"));
        CHECK_EQ(get_resp_body_merged(ctx), "transient_string_12345");
    });

    int ret = server.start("127.0.0.1", 0);
    REQUIRE_EQ(ret, 0);
    const unsigned short port = static_cast<unsigned short>(server.listen_port());
    REQUIRE_GT(port, 0);

    auto *task = WFTaskFactory::create_http_task("http://127.0.0.1:" + std::to_string(port) + "/test_misc", 0, 1, nullptr);
    auto res = coke::sync_wait(coke::HttpAwaiter(task));
    CHECK_EQ(res.state, 0);
    const void *body_ptr = nullptr;
    size_t body_len = 0;
    res.resp.get_parsed_body(&body_ptr, &body_len);
    CHECK_EQ(std::string_view(static_cast<const char*>(body_ptr), body_len), "transient_string_12345");

    server.stop();
}

} // TEST_SUITE("core")
