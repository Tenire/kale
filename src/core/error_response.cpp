#include "kale/core/error_response.h"
#include "kale/core/context.h"
#include "kale/core/http_status.h"
#include "kale/core/http_util.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace kale {

namespace {

bool is_forbidden_error_header(std::string_view key) noexcept {
    static constexpr std::string_view forbidden[] = {
        "content-length",
        "transfer-encoding",
        "connection",
        "upgrade",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "te",
        "trailer",
        "trailers",
        "content-encoding",
        "content-range",
        "host"
    };
    for (std::string_view f : forbidden) {
        if (case_insensitive_equal(key, f)) {
            return true;
        }
    }
    return false;
}

std::string escape_bearer_realm(std::string_view realm) {
    std::string out;
    out.reserve(realm.size() + 2);
    for (char c : realm) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

void escape_json_to(std::string &out, std::string_view s) {
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out.push_back(c);
            }
            break;
        }
    }
}

} // namespace

// ============================================================================
// HttpError implementation
// ============================================================================

HttpError::HttpError(int status, std::string message, std::string error_code, bool is_exposed)
    : std::runtime_error(std::move(message)),
      status_(status),
      error_code_(std::move(error_code)),
      is_exposed_(is_exposed) {}

HttpError::HttpError(HttpStatus status, std::string message, std::string error_code, bool is_exposed)
    : HttpError(static_cast<int>(status), std::move(message), std::move(error_code), is_exposed) {}

HttpError &HttpError::with_header(std::string key, std::string value) {
    if (!is_valid_header_name(key)) {
        throw std::invalid_argument("HttpError: invalid header field name: '" + key + "'");
    }
    if (!is_valid_header_value(key) || !is_valid_header_value(value)) {
        throw std::invalid_argument("HttpError: header key or value contains CRLF/null characters");
    }
    if (is_forbidden_error_header(key)) {
        throw std::invalid_argument("HttpError: forbidden response framing/hop-by-hop header: '" + key + "'");
    }

    for (auto &h : headers_) {
        if (case_insensitive_equal(h.first, key)) {
            h.second = std::move(value);
            return *this;
        }
    }
    headers_.emplace_back(std::move(key), std::move(value));
    return *this;
}

HttpError &HttpError::with_detail(std::string key, std::string value) {
    details_[std::move(key)] = std::move(value);
    return *this;
}

HttpError HttpError::bad_request(std::string message, std::string error_code) {
    return HttpError(static_cast<int>(HttpStatus::BadRequest), std::move(message), std::move(error_code), true);
}

HttpError HttpError::unauthorized(std::string message, std::string error_code, std::string_view realm) {
    HttpError err(static_cast<int>(HttpStatus::Unauthorized), std::move(message), std::move(error_code), true);
    if (!realm.empty()) {
        err.with_header("WWW-Authenticate", "Bearer realm=\"" + escape_bearer_realm(realm) + "\"");
    } else {
        err.with_header("WWW-Authenticate", "Bearer");
    }
    return err;
}

HttpError HttpError::forbidden(std::string message, std::string error_code) {
    return HttpError(static_cast<int>(HttpStatus::Forbidden), std::move(message), std::move(error_code), true);
}

HttpError HttpError::not_found(std::string message, std::string error_code) {
    return HttpError(static_cast<int>(HttpStatus::NotFound), std::move(message), std::move(error_code), true);
}

HttpError HttpError::method_not_allowed(std::string allow_header, std::string message, std::string error_code) {
    HttpError err(static_cast<int>(HttpStatus::MethodNotAllowed), std::move(message), std::move(error_code), true);
    if (!allow_header.empty()) {
        err.with_header("Allow", std::move(allow_header));
    }
    return err;
}

HttpError HttpError::conflict(std::string message, std::string error_code) {
    return HttpError(static_cast<int>(HttpStatus::Conflict), std::move(message), std::move(error_code), true);
}

HttpError HttpError::payload_too_large(std::string message, std::string error_code) {
    return HttpError(static_cast<int>(HttpStatus::PayloadTooLarge), std::move(message), std::move(error_code), true);
}

HttpError HttpError::unprocessable_entity(std::string message, std::string error_code) {
    return HttpError(static_cast<int>(HttpStatus::UnprocessableEntity), std::move(message), std::move(error_code), true);
}

HttpError HttpError::too_many_requests(std::string message, int retry_after_seconds, std::string error_code) {
    HttpError err(static_cast<int>(HttpStatus::TooManyRequests), std::move(message), std::move(error_code), true);
    if (retry_after_seconds > 0) {
        err.with_header("Retry-After", std::to_string(retry_after_seconds));
    }
    return err;
}

HttpError HttpError::internal_error(std::string message, std::string error_code, bool is_exposed) {
    return HttpError(static_cast<int>(HttpStatus::InternalServerError), std::move(message), std::move(error_code), is_exposed);
}

HttpError HttpError::service_unavailable(std::string message, int retry_after_seconds, std::string error_code) {
    HttpError err(static_cast<int>(HttpStatus::ServiceUnavailable), std::move(message), std::move(error_code), true);
    if (retry_after_seconds > 0) {
        err.with_header("Retry-After", std::to_string(retry_after_seconds));
    }
    return err;
}

// ============================================================================
// Core Formatters and Defaults
// ============================================================================

void plain_text_error_formatter(Context &ctx, const ErrorContext &err_ctx) {
    const auto *policy_ptr = ctx.error_policy();
    const bool always_200 = policy_ptr ? policy_ptr->always_http_200 : false;
    ctx.set_status(always_200 ? 200 : err_ctx.status_code);
    ctx.set_content_type("text/plain; charset=utf-8");

    std::string body;
    if (always_200) {
        body = std::to_string(err_ctx.status_code);
        if (!err_ctx.error_code.empty()) {
            body += " [" + std::string(err_ctx.error_code) + "]";
        }
        body += " " + std::string(err_ctx.message) + "\n";
    } else {
        if (!err_ctx.error_code.empty()) {
            body = std::to_string(err_ctx.status_code) + " [" + std::string(err_ctx.error_code) + "] " + std::string(err_ctx.message) + "\n";
        } else {
            body = std::to_string(err_ctx.status_code) + " " + std::string(err_ctx.message) + "\n";
        }
    }
    ctx.body(body);
}

void standard_json_error_formatter(Context &ctx, const ErrorContext &err_ctx) {
    const auto *policy_ptr = ctx.error_policy();
    const bool always_200 = policy_ptr ? policy_ptr->always_http_200 : false;
    ctx.set_status(always_200 ? 200 : err_ctx.status_code);
    ctx.set_content_type("application/json; charset=utf-8");

    std::string out;
    out.reserve(256);
    out += "{\"code\":";
    out += std::to_string(err_ctx.status_code);
    out += ",\"error_code\":\"";
    escape_json_to(out, err_ctx.error_code);
    out += "\",\"message\":\"";
    escape_json_to(out, err_ctx.message);
    out += "\"";

    if (err_ctx.details != nullptr && !err_ctx.details->empty()) {
        out += ",\"details\":{";
        bool first = true;
        for (const auto &[k, v] : *err_ctx.details) {
            if (!first) out += ",";
            first = false;
            out += "\"";
            escape_json_to(out, k);
            out += "\":\"";
            escape_json_to(out, v);
            out += "\"";
        }
        out += "}";
    }
    out += "}";
    ctx.body(out);
}

void problem_details_error_formatter(Context &ctx, const ErrorContext &err_ctx) {
    const auto *policy_ptr = ctx.error_policy();
    const bool always_200 = policy_ptr ? policy_ptr->always_http_200 : false;
    const int http_status = always_200 ? 200 : err_ctx.status_code;

    ctx.set_status(http_status);
    ctx.set_content_type("application/problem+json; charset=utf-8");

    std::string out;
    out.reserve(256);
    out += "{\"type\":\"about:blank\",\"title\":\"";
    escape_json_to(out, status_phrase(err_ctx.status_code));
    out += "\",\"status\":";
    // Per RFC 9457 §3.1.2: "status" in problem details object MUST match HTTP status code
    out += std::to_string(http_status);
    out += ",\"detail\":\"";
    escape_json_to(out, err_ctx.message);
    out += "\"";

    if (!err_ctx.error_code.empty()) {
        out += ",\"error_code\":\"";
        escape_json_to(out, err_ctx.error_code);
        out += "\"";
    }

    if (always_200) {
        // Explicit extension preserving original error status in RPC mode
        out += ",\"error_status\":";
        out += std::to_string(err_ctx.status_code);
    }

    if (err_ctx.details != nullptr && !err_ctx.details->empty()) {
        out += ",\"details\":{";
        bool first = true;
        for (const auto &[k, v] : *err_ctx.details) {
            if (!first) out += ",";
            first = false;
            out += "\"";
            escape_json_to(out, k);
            out += "\":\"";
            escape_json_to(out, v);
            out += "\"";
        }
        out += "}";
    }
    out += "}";
    ctx.body(out);
}

void default_unconfigured_error_formatter(Context &ctx, const ErrorContext &err_ctx) {
    const auto *policy_ptr = ctx.error_policy();
    const bool always_200 = policy_ptr ? policy_ptr->always_http_200 : false;
    ctx.set_status(always_200 ? 200 : err_ctx.status_code);
    ctx.set_content_type("text/plain; charset=utf-8");
    ctx.string(status_phrase(err_ctx.status_code));
}

const ErrorFormatter &default_plain_text_formatter() noexcept {
    static const ErrorFormatter fmt = default_unconfigured_error_formatter;
    return fmt;
}

const ErrorFormatter &default_standard_json_formatter() noexcept {
    static const ErrorFormatter fmt = standard_json_error_formatter;
    return fmt;
}

const ErrorFormatter &default_problem_details_formatter() noexcept {
    static const ErrorFormatter fmt = problem_details_error_formatter;
    return fmt;
}

const ErrorPolicy &default_error_policy() noexcept {
    static const ErrorPolicy pol{};
    return pol;
}

void clear_framing_headers(Context &ctx) noexcept {
    ctx.erase_resp_header("Content-Length");
    ctx.erase_resp_header("Transfer-Encoding");
    ctx.erase_resp_header("Content-Encoding");
    ctx.erase_resp_header("Content-Range");
    ctx.erase_resp_header("Trailer");
    ctx.erase_resp_header("Content-MD5");
    ctx.erase_resp_header("Content-Type");
    ctx.erase_resp_header("ETag");
    ctx.erase_resp_header("Last-Modified");
}

void apply_error_headers(Context &ctx, const HttpError::HeaderList &headers) {
    for (const auto &[k, v] : headers) {
        ctx.set_header(k, v);
    }
}

// ============================================================================
// reply_error implementations
// ============================================================================

namespace {

void reply_error_impl(Context &ctx,
                      const HttpError *err,
                      std::exception_ptr ep,
                      const ErrorFormatter *fmt_override,
                      const ErrorPolicy *pol_override) {
    if (ctx.is_stream() || ctx.is_replied()) {
        if (ctx.is_stream()) {
            ctx.abort_stream();
        }
        if (auto *task = ctx.coke_context().get_task()) {
            task->noreply();
        }
        return;
    }

    struct ScopedConfigGuard {
        Context &ctx_;
        const ErrorFormatter *orig_fmt_;
        const ErrorPolicy *orig_pol_;

        ScopedConfigGuard(Context &ctx, const ErrorFormatter *fmt, const ErrorPolicy *pol)
            : ctx_(ctx), orig_fmt_(ctx.error_formatter()), orig_pol_(ctx.error_policy()) {
            ctx_.set_error_config(fmt, pol);
        }
        ~ScopedConfigGuard() {
            ctx_.set_error_config(orig_fmt_, orig_pol_);
        }
    };

    const ErrorPolicy &policy = pol_override ? *pol_override :
        (ctx.error_policy() ? *ctx.error_policy() : default_error_policy());
    const ErrorFormatter &formatter = (fmt_override && *fmt_override) ? *fmt_override :
        ((ctx.error_formatter() && *ctx.error_formatter()) ? *ctx.error_formatter() : default_plain_text_formatter());

    ScopedConfigGuard guard(ctx, &formatter, &policy);

    ErrorContext err_ctx;
    const HttpError *caught_http = err;
    const std::exception *caught_std = nullptr;

    if (ep) {
        err_ctx.exception = ep;
        try {
            std::rethrow_exception(ep);
        } catch (const HttpError &he) {
            caught_http = &he;
        } catch (const std::exception &se) {
            caught_std = &se;
        } catch (...) {
        }
    }

    if (caught_http) {
        err_ctx.status_code = caught_http->status_code();
        err_ctx.error_code = caught_http->error_code();
        err_ctx.message = caught_http->what();
        err_ctx.is_exposed = caught_http->is_exposed();
        err_ctx.details = &caught_http->details();
        err_ctx.headers = &caught_http->headers();
        err_ctx.raw_exception = caught_http;
    } else if (caught_std) {
        err_ctx.status_code = policy.default_status_code;
        err_ctx.error_code = policy.default_error_code;
        err_ctx.message = caught_std->what();
        err_ctx.is_exposed = false;
        err_ctx.raw_exception = caught_std;
    } else {
        err_ctx.status_code = policy.default_status_code;
        err_ctx.error_code = policy.default_error_code;
        err_ctx.message = "unknown non-std exception";
        err_ctx.is_exposed = false;
        err_ctx.raw_exception = nullptr;
    }

    const std::string_view unredacted_message = err_ctx.message;
    if (!err_ctx.is_exposed && !policy.expose_internal_errors) {
        err_ctx.message = policy.default_blurred_message;
        err_ctx.error_code = policy.default_error_code;
        err_ctx.details = nullptr;
    }

    if (err_ctx.status_code >= 500 && policy.logger.enabled(LogLevel::Error)) {
        LogContext log_ctx{"http_server", ctx.method_string(), ctx.path(), err_ctx.status_code};
        policy.logger.log(LogLevel::Error, log_ctx, unredacted_message);
    }

    clear_framing_headers(ctx);
    if (err_ctx.headers) {
        apply_error_headers(ctx, *err_ctx.headers);
    }

    try {
        formatter(ctx, err_ctx);
    } catch (const std::exception &fmt_ex) {
        if (policy.logger.enabled(LogLevel::Error)) {
            policy.logger.log(LogLevel::Error, "error_formatter", fmt_ex.what());
        }
        clear_framing_headers(ctx);
        ctx.set_status(policy.always_http_200 ? 200 : 500);
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("Internal Server Error\n");
    } catch (...) {
        clear_framing_headers(ctx);
        ctx.set_status(policy.always_http_200 ? 200 : 500);
        ctx.set_content_type("text/plain; charset=utf-8");
        ctx.string("Internal Server Error\n");
    }

    if (!ctx.is_stream() && !ctx.is_replied() && !ctx.has_resp_header("Transfer-Encoding")) {
        ctx.set_header("Content-Length", std::to_string(ctx.resp_body_size()));
    }
}

} // namespace

void reply_error(Context &ctx, const HttpError &err) {
    reply_error_impl(ctx, &err, nullptr, nullptr, nullptr);
}

void reply_error(Context &ctx,
                 const HttpError &err,
                 const ErrorFormatter &formatter,
                 const ErrorPolicy &policy) {
    reply_error_impl(ctx, &err, nullptr, &formatter, &policy);
}

void reply_error(Context &ctx, std::exception_ptr ep) {
    reply_error_impl(ctx, nullptr, ep, nullptr, nullptr);
}

void reply_error(Context &ctx,
                 std::exception_ptr ep,
                 const ErrorFormatter &formatter,
                 const ErrorPolicy &policy) {
    reply_error_impl(ctx, nullptr, ep, &formatter, &policy);
}

} // namespace kale
