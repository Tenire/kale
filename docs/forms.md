# Form Handling: URL-Encoded & Multipart Data

This task guide demonstrates handling HTML form submissions in Kale. It covers direct key-value inspection for `application/x-www-form-urlencoded` requests, validated field ingestion, and inspecting multipart text and file attachments for `multipart/form-data`.

Source code: [`../examples/forms.cpp`](../examples/forms.cpp)

---

## 1. Goal

The `example_forms` service implements a Support Contact and Job Application portal that demonstrates:
- Inspecting URL-encoded parameters using `kale::has_form(...)`, `kale::form(...)`, and `kale::parse_form(...)`.
- Validating incoming form inputs (string bounds, email checks, rating ranges, checkbox flags).
- Multipart boundary parsing and part inspection using `kale::parse_multipart(...)` and `kale::form_file(...)`.
- Inspecting attachment metadata (`name`, `filename`, `content_type`, `size_bytes`, magic bytes).
- Real protocol contracts and error states for missing boundaries, truncated payloads, and size limits.

---

## 2. Build & Run

Kale uses native `xmake` for building targets.

### Build
From the repository root:
```bash
xmake build example_forms
```

### Run
Run on the default port (`8080`):
```bash
xmake run example_forms
```

Run on an explicit port (e.g. `9090`):
```bash
xmake run example_forms 9090
```

Run on an OS-assigned ephemeral port (`0`):
```bash
xmake run example_forms 0
```

---

## 3. URL-Encoded Form Processing

URL-encoded forms (`application/x-www-form-urlencoded`) transmit key-value pairs serialized in the request body as percent-encoded strings (e.g. `name=Alice&email=alice%40example.com`).

The `kale/http/form.h` header provides non-allocating and map-based inspection APIs:

```cpp
#include <map>
#include <string>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/http/form.h"

void inspect_form_fields(const kale::Context &ctx) {
    // Check if a field is present
    bool exists = kale::has_form(ctx, "email");

    // Extract a specific field value (returns empty string if missing)
    std::string email = kale::form(ctx, "email");

    // Parse all non-file form fields into a map
    std::map<std::string, std::string, std::less<>> fields = kale::parse_form(ctx);
    (void)exists;
    (void)email;
    (void)fields;
}
```

---

## 4. Multipart Form Processing (`multipart/form-data`)

Multipart forms transmit structured boundaries separating multiple heterogeneous parts. Each part contains its own headers (`Content-Disposition`, `Content-Type`) and payload data.

The `kale/http/multipart.h` header provides:

```cpp
#include <optional>
#include <vector>

// Convenience headers (active):
#include <kale/core.h>
#include <kale/http.h>

// Precise headers (alternative; comment the convenience group before enabling):
// #include "kale/core/context.h"
// #include "kale/http/multipart.h"

void inspect_multipart_files(const kale::Context &ctx) {
    // Parse all multipart parts into a vector of FormFile
    std::vector<kale::FormFile> parts = kale::parse_multipart(ctx);

    // Lookup the first part matching field name
    std::optional<kale::FormFile> resume = kale::form_file(ctx, "resume");
    (void)parts;
    (void)resume;
}
```

### Zero-Copy Borrowed Buffer Semantics
Notice that `FormFile::data` is a `std::string_view`.
- **Zero Heap Copying**: Kale does not allocate heap buffers for file contents during multipart parsing; `data` directly borrows the memory range from `ctx.req_body()`.
- **Lifetime Limit**: The view remains valid only while the current `Context` exists. Do not store `FormFile::data` across long-lived asynchronous tasks without copying it into an owned `std::string`.

### Disk Persistence Note
Disk persistence (persisting buffered request payloads via asynchronous file writes) belongs to the Uploads scope. See the [`uploads.md`](uploads.md) guide for asynchronous file upload workflows.

---

## 5. Curl Recipes & Expected Responses

### 5.1. Submit Contact Form (POST /submit/contact)
Submit standard URL-encoded fields using `curl -d`:
```bash
curl -s -X POST http://127.0.0.1:8080/submit/contact \
  -d "name=Alice+Smith&email=alice%40example.com&message=Great+framework%21" | jq .
```
Expected Response (`200 OK`):
```json
{
  "status": "received",
  "mode": "url_encoded_direct_inspection",
  "parsed_field_count": 3,
  "fields": {
    "email": "alice@example.com",
    "message": "Great framework!",
    "name": "Alice Smith"
  }
}
```

---

### 5.2. Submit Validated Feedback (POST /submit/feedback)
Submit validated feedback with rating and newsletter subscription:
```bash
curl -s -X POST http://127.0.0.1:8080/submit/feedback \
  -d "user_name=Bob&email=bob%40example.org&category=feature&rating=5&subscribe_newsletter=on" | jq .
```
Expected Response (`200 OK`):
```json
{
  "status": "accepted",
  "user_name": "Bob",
  "email": "bob@example.org",
  "category": "feature",
  "rating": 5,
  "subscribed": true
}
```

---

### 5.3. Submit Job Application with Attachment (POST /submit/application)
Create a temporary PDF file and submit a multipart form with text fields and file attachment:
```bash
# Create dummy sample PDF file
printf "%%PDF-1.4\n1 0 obj<<>>endobj\ntrailer<<>>%%%%EOF\n" > /tmp/sample_cv.pdf

curl -s -X POST http://127.0.0.1:8080/submit/application \
  -F "applicant_name=Carol Danvers" \
  -F "cover_letter=Experienced C++ backend engineer." \
  -F "resume=@/tmp/sample_cv.pdf;type=application/pdf" | jq .

rm -f /tmp/sample_cv.pdf
```
Expected Response (`200 OK`):
```json
{
  "status": "inspected",
  "applicant_name": "Carol Danvers",
  "has_cover_letter": true,
  "resume": {
    "filename": "sample_cv.pdf",
    "content_type": "application/pdf",
    "size_bytes": 45,
    "valid_pdf_magic": true
  },
  "total_parts_inspected": 3,
  "parts": [
    {
      "field_name": "applicant_name",
      "is_file": false,
      "size_bytes": 13
    },
    {
      "field_name": "cover_letter",
      "is_file": false,
      "size_bytes": 33
    },
    {
      "field_name": "resume",
      "is_file": true,
      "size_bytes": 45,
      "filename": "sample_cv.pdf",
      "content_type": "application/pdf"
    }
  ]
}
```

---

## 6. User Protocol Contract & Error Responses

The table below summarizes the contractual behavior for invalid inputs:

| Scenario | HTTP Status | Response Payload | Description |
| :--- | :--- | :--- | :--- |
| Missing / Unsupported `Content-Type` | `415 Unsupported Media Type` | `{"error": "UNSUPPORTED_MEDIA_TYPE", ...}` | Client sent non-form Content-Type |
| Missing Required Form Fields | `400 Bad Request` | `{"error": "MISSING_FORM_FIELDS", ...}` | One or more mandatory fields were omitted |
| Empty Field Value | `400 Bad Request` | `{"error": "EMPTY_FIELD_VALUE", ...}` | Field present but empty string submitted |
| Field Validation Constraint Error | `422 Unprocessable Entity` | `{"error": "VALIDATION_FAILED", "details": [...]}` | Out-of-bounds rating, invalid email, or disallowed category |
| Missing Multipart Boundary | `400 Bad Request` | `{"error": "EMPTY_MULTIPART", ...}` | `boundary=` omitted from `Content-Type` or unclosed |
| Missing Required Attachment | `400 Bad Request` | `{"error": "MISSING_ATTACHMENT", ...}` | No part named `'resume'` present in form |
| Attachment Exceeds 5 MiB Limit | `413 Payload Too Large` | `{"error": "FILE_TOO_LARGE", ...}` | File payload size $> 5 \text{ MiB}$ |

### Example: Validation Failure on Feedback (HTTP 422)
Submitting an out-of-bounds rating (`rating=10`) and invalid category:
```bash
curl -s -X POST http://127.0.0.1:8080/submit/feedback \
  -d "user_name=D&email=notanemail&category=invalid&rating=10" | jq .
```
Expected Response (`422 Unprocessable Entity`):
```json
{
  "error": "VALIDATION_FAILED",
  "details": [
    {
      "field": "user_name",
      "reason": "user_name length must be between 2 and 50 characters"
    },
    {
      "field": "email",
      "reason": "email must be a valid email address"
    },
    {
      "field": "category",
      "reason": "category must be one of: general, bug, feature, billing"
    },
    {
      "field": "rating",
      "reason": "rating must be an integer between 1 and 5"
    }
  ]
}
```

### Example: Missing Attachment in Multipart (HTTP 400)
Submitting multipart form without the required `'resume'` file field:
```bash
curl -s -X POST http://127.0.0.1:8080/submit/application \
  -F "applicant_name=Dave" | jq .
```
Expected Response (`400 Bad Request`):
```json
{
  "error": "MISSING_ATTACHMENT",
  "message": "Multipart form requires a file field named 'resume'"
}
```
