#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Dead-simple HTTP/1.0 GET over the TCP client. No TLS (so http:// only), no
 *  chunked transfer (we ask for Connection: close and read to EOF). Enough to
 *  pull a basic HTML page or text file into a buffer.
 * ===========================================================================*/

/* Fetch URL ("http://host[:port]/path" or "host/path") into out (NUL-terminated,
 * truncated to cap-1). Returns body length on success, -1 on failure.
 *  *status   <- HTTP status code (e.g. 200, 404), if non-NULL.
 *  location  <- value of a redirect's Location header (3xx), if non-NULL. */
int http_get(const char *url, char *out, uint32_t cap,
             int *status, char *location, uint32_t loc_cap);
