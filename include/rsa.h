#pragma once
#include <stdint.h>

/* RSA signature verification for X.509 / TLS. Public-key only (modulus + small
 * exponent). Big-endian byte strings throughout. Supports moduli up to 4096
 * bits. h384=1 selects SHA-384, else SHA-256. Returns 0 if the signature
 * verifies, -1 otherwise. */

int rsa_verify_pkcs1(const uint8_t *n, uint32_t nlen,
                     const uint8_t *e, uint32_t elen,
                     const uint8_t *sig, uint32_t siglen,
                     const uint8_t *hash, uint32_t hashlen, int h384);

int rsa_verify_pss(const uint8_t *n, uint32_t nlen,
                   const uint8_t *e, uint32_t elen,
                   const uint8_t *sig, uint32_t siglen,
                   const uint8_t *hash, uint32_t hashlen, int h384);
