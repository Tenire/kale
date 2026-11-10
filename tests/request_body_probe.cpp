/**
 * Kale Core Native Request-Body Capability Probe
 *
 * Scope & Purpose:
 * Exercises native Workflow / Coke BasicServer request body handling to distinguish
 * empirical runtime observations from source-based architectural capability limits.
 *
 * Probe Sections:
 * 1. Standard Server Body Buffering & Dispatch Timing:
 *    Empirically measures body visibility at handler entry and verifies publication ordering.
 * 2. Protected HttpRequest::append Customization & Deterministic Arrival Handshake:
 *    Empirically verifies whether handler dispatch occurs on partial arrival using a
 *    deterministic server-side partial body arrival handshake (inspecting native http_parser_t
 *    msgsize vs header_offset) before sending remainder, inspects reactor execution context,
 *    and validates exact bytes consumed and complete HTTP response status/body framing.
 * 3. Premature Client Disconnect Lifetime:
 *    Demonstrates server task deletion behavior upon client disconnect before body completion,
 *    awaiting native request object destruction publication after confirmed partial append.
 */

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <charconv>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <openssl/crypto.h>

#include "kale/core/http_server.h"
#include "kale/core/context.h"
#include "coke/task.h"
#include "coke/sleep.h"
#include "coke/wait.h"
#include "coke/net/basic_server.h"
#include "workflow/HttpMessage.h"
#include "workflow/WFServer.h"
#include "workflow/HttpUtil.h"

#define PROBE_CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "PROBE CHECK FAILED: " << (msg) << " (" #cond ") at " \
                      << __FILE__ << ":" << __LINE__ << std::endl; \
            return EXIT_FAILURE; \
        } \
    } while (0)

namespace {

using namespace std::chrono_literals;

class ScopedFd {
public:
    ScopedFd() noexcept = default;
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}
    ~ScopedFd() { reset(); }

    ScopedFd(const ScopedFd &) = delete;
    ScopedFd &operator=(const ScopedFd &) = delete;

    ScopedFd(ScopedFd &&other) noexcept : fd_(other.release()) {}
    ScopedFd &operator=(ScopedFd &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    operator int() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};
pid_t get_tid() {
#if defined(SYS_gettid)
    return static_cast<pid_t>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// Socket helper utilities with EINTR handling and bounded deadlines
// ---------------------------------------------------------------------------
int connect_raw_tcp(unsigned short port, std::chrono::milliseconds timeout = 2000ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);

    while (true) {
        int ret = ::connect(fd, reinterpret_cast<struct sockaddr*>(&sin), sizeof(sin));
        if (ret == 0) break;
        if (errno == EINTR) continue;
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const void *buf, size_t len, std::chrono::milliseconds timeout = 2000ms) {
    const char *p = static_cast<const char *>(buf);
    size_t remaining = len;
    auto start = std::chrono::steady_clock::now();

    while (remaining > 0) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            return false;
        }
        ssize_t n = ::send(fd, p, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

std::string read_all_response(int fd, std::chrono::milliseconds timeout = 2000ms) {
    std::string out;
    char buf[4096];
    auto start = std::chrono::steady_clock::now();

    while (true) {
        if (std::chrono::steady_clock::now() - start > timeout) {
            break;
        }
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

struct HttpResponseParsed {
    int status_code{0};
    std::string body;
    bool valid{false};
};

HttpResponseParsed parse_http_response(std::string_view raw) {
    HttpResponseParsed res;
    if (raw.empty()) return res;

    auto line_end = raw.find("\r\n");
    if (line_end == std::string_view::npos) return res;

    std::string_view status_line = raw.substr(0, line_end);
    auto space1 = status_line.find(' ');
    if (space1 != std::string_view::npos && space1 + 1 < status_line.size()) {
        int code = 0;
        const char *first = status_line.data() + space1 + 1;
        const char *last = status_line.data() + status_line.size();
        auto [ptr, ec] = std::from_chars(first, last, code);
        (void)ptr;
        if (ec == std::errc{}) {
            res.status_code = code;
        }
    }

    auto body_start = raw.find("\r\n\r\n");
    if (body_start != std::string_view::npos) {
        res.body = std::string(raw.substr(body_start + 4));
        res.valid = (res.status_code > 0);
    }
    return res;
}

// ---------------------------------------------------------------------------
// Standard Server Observation Structure
// ---------------------------------------------------------------------------
struct ServerObservation {
    std::chrono::steady_clock::time_point handler_entered_time;
    size_t body_size_at_entry{0};
    std::atomic<bool> handler_completed{false};
};

// ---------------------------------------------------------------------------
// Custom HttpRequest Inspection Class: Overriding protected HttpRequest::append
// ---------------------------------------------------------------------------
class CustomProbeHttpRequest : public protocol::HttpRequest {
public:
    static inline std::atomic<size_t> append_invocation_count{0};
    static inline std::atomic<size_t> total_bytes_appended{0};
    static inline std::atomic<pid_t> last_append_tid{0};
    static inline std::atomic<bool> partial_body_arrived{false};
    static inline std::atomic<bool> proc_called{false};
    static inline std::atomic<pid_t> handler_tid{0};

    static void reset() {
        append_invocation_count.store(0, std::memory_order_relaxed);
        total_bytes_appended.store(0, std::memory_order_relaxed);
        last_append_tid.store(0, std::memory_order_relaxed);
        partial_body_arrived.store(false, std::memory_order_relaxed);
        proc_called.store(false, std::memory_order_relaxed);
        handler_tid.store(0, std::memory_order_relaxed);
    }

protected:
    int append(const void *buf, size_t *size) override {
        last_append_tid.store(get_tid(), std::memory_order_relaxed);
        int ret = this->HttpRequest::append(buf, size);

        if (ret >= 0) {
            append_invocation_count.fetch_add(1, std::memory_order_relaxed);
            total_bytes_appended.fetch_add(*size, std::memory_order_relaxed);

            // Deterministic partial-body arrival handshake:
            if (this->is_header_complete()) {
                const http_parser_t *p = this->get_parser();
                if (p && p->msgsize > p->header_offset) {
                    partial_body_arrived.store(true, std::memory_order_release);
                }
            }
        }
        return ret;
    }
};

// ---------------------------------------------------------------------------
// Disconnect Inspection Class: Request Destruction Publication
// ---------------------------------------------------------------------------
class DisconnectProbeHttpRequest : public protocol::HttpRequest {
public:
    static inline std::atomic<bool> partial_body_appended{false};
    static inline std::atomic<bool> request_destructed{false};
    static inline std::atomic<size_t> bytes_appended{0};

    static void reset() {
        partial_body_appended.store(false, std::memory_order_relaxed);
        request_destructed.store(false, std::memory_order_relaxed);
        bytes_appended.store(0, std::memory_order_relaxed);
    }

    ~DisconnectProbeHttpRequest() override {
        // Native destructor execution published when WFServerTask is deleted
        request_destructed.store(true, std::memory_order_release);
    }

protected:
    int append(const void *buf, size_t *size) override {
        int ret = this->HttpRequest::append(buf, size);
        if (ret >= 0) {
            bytes_appended.fetch_add(*size, std::memory_order_relaxed);
            if (this->is_header_complete()) {
                const http_parser_t *p = this->get_parser();
                if (p && p->msgsize > p->header_offset) {
                    partial_body_appended.store(true, std::memory_order_release);
                }
            }
        }
        return ret;
    }
};

} // namespace

int main() {
    // Preserve OpenSSL initialization ordering before Workflow worker creation
    if (OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr) != 1) {
        return EXIT_FAILURE;
    }

    std::cout << "================================================================================" << std::endl;
    std::cout << "Kale Core Native Request-Body Capability Probe" << std::endl;
    std::cout << "Backend: Workflow-WS / Coke BasicServer" << std::endl;
    std::cout << "================================================================================" << std::endl;

    // ---------------------------------------------------------------------------
    // Section 1: Standard Server Handler Dispatch Timing vs Full Buffering
    // ---------------------------------------------------------------------------
    std::cout << "\n[Section 1] Standard Server Body Buffering & Dispatch Timing..." << std::endl;
    {
        kale::HttpServer server;
        ServerObservation obs;
        std::chrono::steady_clock::time_point chunk1_sent_time;

        const size_t total_payload_size = 64 * 1024; // 64 KiB
        const size_t chunk1_size = 1024;             // 1 KiB
        const size_t chunk2_size = total_payload_size - chunk1_size; // 63 KiB

        server.POST("/probe/slow_body", [&](kale::Context &ctx) {
            obs.handler_entered_time = std::chrono::steady_clock::now();
            obs.body_size_at_entry = ctx.req_body().size();
            obs.handler_completed.store(true, std::memory_order_release);
            ctx.string("ok_probe_1");
        });

        int ret = server.start("127.0.0.1", 0);
        PROBE_CHECK(ret == 0, "server start must succeed");
        unsigned short port = static_cast<unsigned short>(server.listen_port());
        PROBE_CHECK(port > 0, "server listen port must be valid");

        ScopedFd fd(connect_raw_tcp(port));
        PROBE_CHECK(fd >= 0, "client connect must succeed");

        std::string headers = "POST /probe/slow_body HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Content-Length: " + std::to_string(total_payload_size) + "\r\n"
                              "Connection: close\r\n\r\n";
        PROBE_CHECK(send_all(fd, headers.data(), headers.size()), "failed to send request headers");

        // Send chunk 1 (1 KiB)
        std::string chunk1(chunk1_size, 'A');
        chunk1_sent_time = std::chrono::steady_clock::now();
        PROBE_CHECK(send_all(fd, chunk1.data(), chunk1.size()), "failed to send chunk 1");

        // Check if handler has entered before chunk 2 is sent
        std::this_thread::sleep_for(150ms);
        bool early_completed = obs.handler_completed.load(std::memory_order_acquire);
        std::cout << "  - Observation A (After sending 1 KiB of 64 KiB):" << std::endl;
        std::cout << "    * Handler completed/entered early? " << (early_completed ? "YES" : "NO") << std::endl;
        PROBE_CHECK(!early_completed, "Standard server MUST NOT dispatch handler before full body arrival");

        // Send chunk 2 (remaining 63 KiB)
        std::string chunk2(chunk2_size, 'B');
        PROBE_CHECK(send_all(fd, chunk2.data(), chunk2.size()), "failed to send chunk 2");

        // Await full HTTP response over TCP socket, confirming server processing completed
        std::string raw_resp = read_all_response(fd);
        fd.reset();

        HttpResponseParsed resp = parse_http_response(raw_resp);
        PROBE_CHECK(resp.valid, "Response must be valid HTTP");
        PROBE_CHECK(resp.status_code == 200, "Response status code must be 200");
        PROBE_CHECK(resp.body == "ok_probe_1", "Response body must match expected");

        bool final_completed = obs.handler_completed.load(std::memory_order_acquire);
        PROBE_CHECK(final_completed, "Handler must complete after full body arrival");

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(obs.handler_entered_time - chunk1_sent_time).count();
        size_t recorded_size = obs.body_size_at_entry;

        std::cout << "  - Observation B (After sending remaining 63 KiB & receiving response):" << std::endl;
        std::cout << "    * Time from chunk 1 sent to handler entry: " << elapsed_ms << " ms" << std::endl;
        std::cout << "    * Body size visible at handler entry: " << recorded_size << " bytes (expected: " << total_payload_size << ")" << std::endl;
        PROBE_CHECK(recorded_size == total_payload_size, "Handler must observe 100% buffered body upon entry");
        std::cout << "  -> Empirical observation: Standard Workflow HTTP server buffers 100% of body before handler entry." << std::endl;

        server.stop();
    }

    // ---------------------------------------------------------------------------
    // Section 2: Protected HttpRequest::append Customization & Deterministic Arrival Handshake
    // ---------------------------------------------------------------------------
    std::cout << "\n[Section 2] Custom HttpRequest::append & Deterministic Partial-Body Handshake..." << std::endl;
    {
        CustomProbeHttpRequest::reset();
        WFServerParams params = SERVER_PARAMS_DEFAULT;
        params.request_size_limit = 1024 * 1024;

        coke::BasicServer<CustomProbeHttpRequest, protocol::HttpResponse> server(
            params,
            [&](coke::ServerContext<CustomProbeHttpRequest, protocol::HttpResponse> ctx) -> coke::Task<void> {
                CustomProbeHttpRequest::handler_tid.store(get_tid(), std::memory_order_release);
                CustomProbeHttpRequest::proc_called.store(true, std::memory_order_release);

                auto &resp = ctx.get_resp();
                resp.set_http_version("HTTP/1.1");
                protocol::HttpUtil::set_response_status(&resp, HttpStatusOK);
                resp.append_output_body("ok_probe_2");
                resp.add_header_pair("Content-Length", "10");
                resp.add_header_pair("Connection", "close");
                co_await ctx.reply();
            }
        );

        int start_ret = server.start(0);
        PROBE_CHECK(start_ret == 0, "BasicServer start must succeed");
        struct sockaddr_in sin{};
        socklen_t len = sizeof(sin);
        int addr_ret = server.get_listen_addr(reinterpret_cast<struct sockaddr*>(&sin), &len);
        PROBE_CHECK(addr_ret == 0, "get_listen_addr must succeed");
        unsigned short port = ntohs(sin.sin_port);
        PROBE_CHECK(port > 0, "server listen port must be valid");

        ScopedFd fd(connect_raw_tcp(port));
        PROBE_CHECK(fd >= 0, "client connect must succeed");

        const size_t total_size = 32 * 1024; // 32 KiB
        const size_t chunk1_size = 1024;     // 1 KiB
        const size_t chunk2_size = total_size - chunk1_size; // 31 KiB

        std::string headers = "POST / HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Content-Length: " + std::to_string(total_size) + "\r\n"
                              "Connection: close\r\n\r\n";
        PROBE_CHECK(send_all(fd, headers.data(), headers.size()), "failed to send headers");

        // Send chunk 1 (1 KiB)
        std::string chunk1(chunk1_size, 'X');
        PROBE_CHECK(send_all(fd, chunk1.data(), chunk1.size()), "failed to send chunk 1");

        // Deterministic partial-body arrival handshake
        bool partial_arrived = false;
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() <= deadline) {
            if (CustomProbeHttpRequest::partial_body_arrived.load(std::memory_order_acquire)) {
                partial_arrived = true;
                break;
            }
            std::this_thread::yield();
        }
        PROBE_CHECK(partial_arrived, "Deterministic partial body arrival handshake timed out");

        bool proc_dispatched_early = CustomProbeHttpRequest::proc_called.load(std::memory_order_acquire);
        std::cout << "  - Observation A (Deterministic Partial-Body Arrival Handshake confirmed BEFORE sending remainder):" << std::endl;
        std::cout << "    * append() confirmed parsing of chunk 1 (" << CustomProbeHttpRequest::total_bytes_appended.load() << " bytes received so far)." << std::endl;
        std::cout << "    * Has server handler (processor) been dispatched? " << (proc_dispatched_early ? "YES" : "NO") << std::endl;
        PROBE_CHECK(!proc_dispatched_early, "Handler MUST NOT be dispatched upon partial body arrival");

        // Send chunk 2 (remaining 31 KiB)
        std::string chunk2(chunk2_size, 'Y');
        PROBE_CHECK(send_all(fd, chunk2.data(), chunk2.size()), "failed to send chunk 2");

        std::string raw_resp = read_all_response(fd);
        fd.reset();

        HttpResponseParsed resp = parse_http_response(raw_resp);
        PROBE_CHECK(resp.valid, "Response must be valid HTTP");
        PROBE_CHECK(resp.status_code == 200, "Response status code must be 200");
        PROBE_CHECK(resp.body == "ok_probe_2", "Response body must match expected");

        server.stop();

        bool proc_dispatched_final = CustomProbeHttpRequest::proc_called.load(std::memory_order_acquire);
        PROBE_CHECK(proc_dispatched_final, "Handler must be dispatched after total body arrival");

        pid_t append_tid = CustomProbeHttpRequest::last_append_tid.load(std::memory_order_acquire);
        pid_t handler_tid = CustomProbeHttpRequest::handler_tid.load(std::memory_order_acquire);
        size_t append_calls = CustomProbeHttpRequest::append_invocation_count.load(std::memory_order_acquire);
        size_t appended_bytes = CustomProbeHttpRequest::total_bytes_appended.load(std::memory_order_acquire);
        size_t total_expected_bytes = headers.size() + total_size;

        std::cout << "  - Observation B (After full body transmission):" << std::endl;
        std::cout << "    * Total append() invocations: " << append_calls << std::endl;
        std::cout << "    * Total bytes processed by append(): " << appended_bytes << " (expected: " << total_expected_bytes << ")" << std::endl;
        std::cout << "    * append() executing thread ID (Poller reactor): " << append_tid << std::endl;
        std::cout << "    * Handler executing thread ID (Worker pool):     " << handler_tid << std::endl;
        PROBE_CHECK(appended_bytes == total_expected_bytes, "Exact bytes consumed by append must match total sent");
        PROBE_CHECK(append_tid != 0 && handler_tid != 0, "TIDs must be recorded");
        PROBE_CHECK(append_tid != handler_tid, "Reactor thread and worker thread must be distinct");

        std::cout << "  -> Observation: Selected ordinary HTTP server path lacks an identified public asynchronous ingress extraction/pause-resume/lifetime bridge." << std::endl;
    }

    // ---------------------------------------------------------------------------
    // Section 3: Premature Client Disconnect Lifetime Check (Destruction Publication)
    // ---------------------------------------------------------------------------
    std::cout << "\n[Section 3] Premature Client Disconnect Lifetime Check..." << std::endl;
    {
        DisconnectProbeHttpRequest::reset();
        WFServerParams params = SERVER_PARAMS_DEFAULT;
        params.request_size_limit = 1024 * 1024;

        std::atomic<bool> handler_called{false};

        coke::BasicServer<DisconnectProbeHttpRequest, protocol::HttpResponse> server(
            params,
            [&](coke::ServerContext<DisconnectProbeHttpRequest, protocol::HttpResponse> ctx) -> coke::Task<void> {
                handler_called.store(true, std::memory_order_release);
                auto &resp = ctx.get_resp();
                resp.set_http_version("HTTP/1.1");
                protocol::HttpUtil::set_response_status(&resp, HttpStatusOK);
                resp.append_output_body("unexpected_call");
                resp.add_header_pair("Content-Length", "15");
                resp.add_header_pair("Connection", "close");
                co_await ctx.reply();
            }
        );

        int start_ret = server.start(0);
        PROBE_CHECK(start_ret == 0, "BasicServer start must succeed");
        struct sockaddr_in sin{};
        socklen_t len = sizeof(sin);
        int addr_ret = server.get_listen_addr(reinterpret_cast<struct sockaddr*>(&sin), &len);
        PROBE_CHECK(addr_ret == 0, "get_listen_addr must succeed");
        unsigned short port = ntohs(sin.sin_port);
        PROBE_CHECK(port > 0, "server listen port must be valid");

        ScopedFd fd(connect_raw_tcp(port));
        PROBE_CHECK(fd >= 0, "client connect must succeed");

        // Send headers declaring 100 KiB, but send only 1 KiB
        std::string headers = "POST / HTTP/1.1\r\n"
                              "Host: 127.0.0.1\r\n"
                              "Content-Length: 102400\r\n"
                              "Connection: close\r\n\r\n";
        PROBE_CHECK(send_all(fd, headers.data(), headers.size()), "failed to send headers");
        std::string partial_body(1024, 'D');
        PROBE_CHECK(send_all(fd, partial_body.data(), partial_body.size()), "failed to send partial body");

        // Await deterministic partial body append before aborting socket
        bool partial_appended = false;
        auto append_deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() <= append_deadline) {
            if (DisconnectProbeHttpRequest::partial_body_appended.load(std::memory_order_acquire)) {
                partial_appended = true;
                break;
            }
            std::this_thread::yield();
        }
        PROBE_CHECK(partial_appended, "Section 3 partial body arrival handshake timed out");

        // Abort connection mid-body
        fd.reset();

        // Await native request destruction publication
        bool destructed = false;
        auto destruct_deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() <= destruct_deadline) {
            if (DisconnectProbeHttpRequest::request_destructed.load(std::memory_order_acquire)) {
                destructed = true;
                break;
            }
            std::this_thread::yield();
        }
        PROBE_CHECK(destructed, "Request destruction publication timed out");

        bool called = handler_called.load(std::memory_order_acquire);
        size_t disconnect_bytes = DisconnectProbeHttpRequest::bytes_appended.load(std::memory_order_acquire);
        std::cout << "  - Observation A (Native request destruction published after aborting 1 KiB of 100 KiB):" << std::endl;
        std::cout << "    * Confirmed native request object destruction? YES" << std::endl;
        std::cout << "    * Partial bytes appended before abort: " << disconnect_bytes << std::endl;
        std::cout << "    * Was server handler invoked on disconnect? " << (called ? "YES" : "NO") << std::endl;
        PROBE_CHECK(disconnect_bytes > 0, "Partial bytes must be appended before abort");
        PROBE_CHECK(!called, "Workflow server MUST NOT dispatch handler for aborted incomplete requests");

        std::cout << "  -> Empirical observation: Disconnect before completion destroys task without handler invocation." << std::endl;

        server.stop();
    }

    std::cout << "\n================================================================================" << std::endl;
    std::cout << "PROBE COMPLETE: All native test sections succeeded." << std::endl;
    std::cout << "================================================================================" << std::endl;

    return 0;
}
