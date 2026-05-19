#pragma once
/*
 * Https.h — HTTPS client helper with TLS state in IRAM second heap
 *
 * Performs HTTPS requests using BearSSL with all TLS allocations directed
 * to the IRAM heap (enabled via PIO_FRAMEWORK_ARDUINO_MMU_CACHE16_IRAM48_SECHEAP_SHARED).
 * The response body is copied back into the caller's DRAM-allocated String.
 *
 * Uses setInsecure() — no certificate validation. Suitable for webhook
 * endpoints where the URL itself acts as the secret.
 */

#include <Arduino.h>

// Perform an HTTPS request.
//   method       — "POST", "GET", "PUT", etc.
//   url          — must start with "https://"
//   contentType  — nullptr/empty to skip Content-Type header
//   body         — request body (may be empty for GET)
//   authHeader   — optional full header line, e.g. "Authorization: Bearer xyz"
//                  nullptr/empty to skip
//   timeoutMs    — total per-request timeout
//   responseOut  — optional, receives response body (DRAM allocated)
//
// Returns HTTP status code (e.g. 200) on success, or a negative value on
// transport/setup error.
int httpsRequest(const char* method,
                 const char* url,
                 const char* contentType,
                 const char* body,
                 const char* authHeader,
                 uint32_t timeoutMs,
                 String* responseOut);
