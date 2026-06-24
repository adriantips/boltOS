#pragma once
#include <stdint.h>

/* DEFLATE (RFC 1951) and gzip (RFC 1952) decompression for the HTTP layer.
 * Both write up to dcap bytes into dst and set *dlen to the produced length.
 * Return 0 on success, -1 on malformed input or output overflow. */
int inflate_raw(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap, uint32_t *dlen);
int gunzip(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap, uint32_t *dlen);
