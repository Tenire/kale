/**
 * @file middlewares.h
 * @brief Convenience aggregation header for Kale HTTP middleware interfaces.
 * Prefer precise headers for individual components.
 */
#ifndef KALE_MIDDLEWARES_H
#define KALE_MIDDLEWARES_H

#include <kale/middleware/access_log.h>
#include <kale/middleware/compression.h>
#include <kale/middleware/cors.h>
#include <kale/middleware/error_handler.h>
#include <kale/middleware/prometheus.h>
#include <kale/middleware/rate_limiter.h>
#include <kale/middleware/request_id.h>
#include <kale/middleware/security_headers.h>

#endif // KALE_MIDDLEWARES_H
