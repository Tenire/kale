# Resumable Chunked Uploads & Direct Offset Persistence

This guide explains how to implement high-performance, resumable multi-part file uploads in Kale using `ResumableUploadCoordinator`, direct non-blocking offset writes via `coke::pwrite`, and end-to-end SHA-256 checksum verification.

The complete runnable code for this guide is located at [`examples/resumable_upload.cpp`](../examples/resumable_upload.cpp).

---

## Building and Running

Compile and launch the resumable upload example with native `xmake`:

```bash
# Build the binary target
xmake build example_resumable_upload

# Run on default port 8080 from repository root
xmake run example_resumable_upload

# Or run with an OS-assigned ephemeral port
xmake run example_resumable_upload 0
```

### Destination Storage Directory

By default, parts are written to `build/example-uploads/resumable` relative to the current working directory. You can customize the path via `KALE_EXAMPLE_UPLOAD_DIR`:

```bash
KALE_EXAMPLE_UPLOAD_DIR=/tmp/storage xmake run example_resumable_upload 8080
```

When started, the service prints:
```
Listening on http://127.0.0.1:8080/
```

Press `Ctrl+C` or send `SIGTERM` to gracefully stop the service.

---

## Direct Offset Architecture & Partitioned Ingress

Kale processes uploads as bounded buffered per-request part payloads with asynchronous direct-offset persistence across requests; there is no single-request ingress streaming. Instead, large uploads are partitioned into bounded part requests.

Traditional multi-part upload implementations store each part in a temporary file and concatenate all temporary files sequentially once all parts arrive. This post-upload merge introduces substantial disk I/O, doubles storage requirements during the merge, and introduces a lengthy latency barrier on finalization.

Kale's `ResumableUploadCoordinator` uses **Direct Offset Asynchronous Writing**:

```
Client Part 1 [Offset 0x000000] ----> coke::pwrite(fd, offset 0)  ----+
                                                                      |
Client Part 2 [Offset 0x100000] ----> coke::pwrite(fd, offset 1MB) ---+--> Preallocated Target File
                                                                      |    (ftruncate)
Client Part 3 [Offset 0x200000] ----> coke::pwrite(fd, offset 2MB) ---+
```

1. **Logical Sizing via `ftruncate`**: When a session is initialized, the server invokes `ftruncate` to set the target file's logical length equal to the declared `total_size`. This establishes file boundaries and enables out-of-order writes at arbitrary offsets without physical contiguous block preallocation.
2. **Out-of-Order Direct Writing**: Each part writes directly to its predetermined file offset `(part_number - 1) * part_size` using non-blocking asynchronous `coke::pwrite`.
3. **No Concatenation Barrier**: When all parts have arrived, the file is already fully assembled on disk. `complete_session` simply syncs disk buffers (`fsync`) and verifies the overall checksum.

---

## Architecture & Minimal Libraries

| Component | Header | Library | Description |
|---|---|---|---|
| `ResumableUploadCoordinator` | `#include "kale/http/resumable_upload.h"` | `kale_http` | Manages active upload sessions and disk offset writes |
| `ChecksumVerifier` | `#include "kale/core/checksum.h"` | `kale_core` | Abstract interface for streaming checksums |
| `EvpChecksum` | `#include "kale/security/evp_checksum.h"` | `kale_security` | OpenSSL EVP SHA-256 streaming digest adapter |
| `Context` | `#include "kale/core/context.h"` | `kale_core` | Core request/response context |
| `Json` | `#include "kale/http/json.h"` | `kale_http` | JSON parsing and serialization |

*Note: For applications preferring CRC-32 over OpenSSL EVP, `#include "kale/core/crc32_checksum.h"` (`kale_core`) provides `Crc32Checksum` conforming to `ChecksumVerifier`.*


### Core Coordinator Excerpt

```cpp
#include <memory>
#include <string>
#include <string_view>

#include "coke/task.h"

// Convenience headers (active):
#include <kale/http.h>
#include <kale/security.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/http/resumable_upload.h"
// #include "kale/security/evp_checksum.h"

coke::Task<int> process_resumable_part(
    std::shared_ptr<kale::ResumableUploadCoordinator> coordinator,
    const std::string &upload_id,
    uint32_t part_number,
    std::string_view part_data,
    const std::string &expected_sha256
) {
    kale::EvpChecksum verifier("SHA256");
    co_return co_await coordinator->write_part(
        upload_id,
        part_number,
        part_data,
        expected_sha256,
        expected_sha256.empty() ? nullptr : &verifier
    );
}
```
---

## In-Memory Coordination & Persistence Limits

- **In-Memory Session State**: `ResumableUploadCoordinator` maintains session descriptors (open file descriptors, total part counts, and completed part bitsets) in memory.
- **Process Termination**: If the server terminates while an upload session is incomplete, in-memory tracking is discarded. The pre-allocated file remains on disk until cleaned up or overwritten by a re-initialized session.
- **Concurrent Part Safety**: Multiple worker threads can write distinct parts of the same session concurrently because `coke::pwrite` writes to independent file offsets without altering the shared file descriptor seek position.

---

## Step-by-Step `curl` Walkthrough

### Step 1: Create an Upload Session

Initialize an upload session declaring the target filename, total size (e.g. 2,097,152 bytes / 2 MB), and part size (e.g. 1,048,576 bytes / 1 MB):

```bash
curl -i -X POST http://127.0.0.1:8080/api/resumable/sessions \
     -H "Content-Type: application/json" \
     -d '{
       "upload_id": "demo-sess-001",
       "filename": "archive.bin",
       "total_size": 2097152,
       "part_size": 1048576
     }'
```

Response:
```http
HTTP/1.1 201 Created
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "created",
  "upload_id": "demo-sess-001",
  "target_path": "build/example-uploads/resumable/demo-sess-001_archive.bin",
  "total_size": 2097152,
  "part_size": 1048576,
  "total_parts": 2
}
```

### Step 2: Upload Parts with SHA-256 Verification

Prepare two 1 MB binary slices:
```bash
# Generate two test slices
head -c 1048576 < /dev/urandom > part1.bin
head -c 1048576 < /dev/urandom > part2.bin

# Calculate SHA-256 for part 1
PART1_SHA256=$(sha256sum part1.bin | awk '{print $1}')
```

Upload Part 1 with expected SHA-256 header:
```bash
curl -i -X PUT http://127.0.0.1:8080/api/resumable/sessions/demo-sess-001/parts/1 \
     -H "Content-Type: application/octet-stream" \
     -H "X-Checksum-SHA256: $PART1_SHA256" \
     --data-binary @part1.bin
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "uploaded",
  "upload_id": "demo-sess-001",
  "part_number": 1,
  "bytes_received": 1048576
}
```

Upload Part 2:
```bash
curl -i -X PUT http://127.0.0.1:8080/api/resumable/sessions/demo-sess-001/parts/2 \
     -H "Content-Type: application/octet-stream" \
     --data-binary @part2.bin
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "uploaded",
  "upload_id": "demo-sess-001",
  "part_number": 2,
  "bytes_received": 1048576
}
```

### Step 3: Query Upload Session Status

```bash
curl -i http://127.0.0.1:8080/api/resumable/sessions/demo-sess-001
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "upload_id": "demo-sess-001",
  "target_path": "build/example-uploads/resumable/demo-sess-001_archive.bin",
  "total_size": 2097152,
  "part_size": 1048576,
  "total_parts": 2,
  "completed_parts": [1, 2],
  "is_completed": true
}
```

### Step 4: Finalize the Upload

Call `complete` to compute the full file SHA-256 checksum, flush to disk, and close the session:

```bash
curl -i -X POST http://127.0.0.1:8080/api/resumable/sessions/demo-sess-001/complete
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "completed",
  "upload_id": "demo-sess-001",
  "sha256": "3a8b49c..."
}
```

### Step 5: Aborting an Incomplete Session

To cancel an upload and delete the pre-allocated disk file:

```bash
curl -i -X DELETE http://127.0.0.1:8080/api/resumable/sessions/demo-sess-001
```

Response:
```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "status": "aborted",
  "upload_id": "demo-sess-001"
}
```

---

## Checksum Verification Errors

If client-supplied data does not match `X-Checksum-SHA256`, the part is rejected immediately with `422 Unprocessable Entity` before committing data to disk:

```bash
curl -i -X PUT http://127.0.0.1:8080/api/resumable/sessions/demo-sess-001/parts/1 \
     -H "X-Checksum-SHA256: badbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadbadb" \
     --data-binary @part1.bin
```

Response:
```http
HTTP/1.1 422 Unprocessable Entity
Content-Type: application/json; charset=utf-8
Server: Kale

{
  "error": "SHA-256 checksum mismatch for part 1"
}
```
