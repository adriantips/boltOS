#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Freestanding crypto primitives backing the TLS 1.2 client (net/tls.c):
 *  SHA-256, HMAC-SHA256, AES-128, AES-128-GCM and X25519. Pure integer code --
 *  no SSE/x87, no libc beyond mem*. Not constant-time-hardened; this is a hobby
 *  OS, and the TLS layer skips certificate verification anyway.
 * ===========================================================================*/

/* ---- SHA-256 ----------------------------------------------------------- */
typedef struct {
    uint32_t h[8];
    uint64_t total;        /* message length in bytes  */
    uint8_t  buf[64];
    uint32_t n;            /* bytes buffered           */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, uint32_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, uint32_t len, uint8_t out[32]);

/* ---- HMAC-SHA256 ------------------------------------------------------- */
void hmac_sha256(const uint8_t *key, uint32_t klen,
                 const uint8_t *msg, uint32_t mlen, uint8_t out[32]);

/* ---- SHA-512 / SHA-384 (for the AES-256-GCM-SHA384 cipher family) ------ */
typedef struct {
    uint64_t h[8];
    uint64_t total;        /* message length in bytes  */
    uint8_t  buf[128];
    uint32_t n;            /* bytes buffered           */
} sha512_ctx;
void sha384_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, uint32_t len);
void sha384_final(sha512_ctx *c, uint8_t out[48]);
void hmac_sha384(const uint8_t *key, uint32_t klen,
                 const uint8_t *msg, uint32_t mlen, uint8_t out[48]);

/* ---- AES-128 (encrypt only; GCM needs the forward cipher both ways) ---- */
typedef struct { uint8_t rk[176]; } aes128_ctx;   /* 11 * 16-byte round keys */
void aes128_init(aes128_ctx *c, const uint8_t key[16]);
void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]);

/* ---- AES-256 (encrypt only) -------------------------------------------- */
typedef struct { uint8_t rk[240]; } aes256_ctx;   /* 15 * 16-byte round keys */
void aes256_init(aes256_ctx *c, const uint8_t key[32]);
void aes256_encrypt(const aes256_ctx *c, const uint8_t in[16], uint8_t out[16]);

/* ---- AES-128-GCM (12-byte IV, 16-byte tag) ----------------------------- */
void aes_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                  const uint8_t *aad, uint32_t aadlen,
                  const uint8_t *pt, uint32_t ptlen,
                  uint8_t *ct, uint8_t tag[16]);
/* returns 0 if the tag verifies, -1 otherwise (pt is still filled either way) */
int  aes_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                  const uint8_t *aad, uint32_t aadlen,
                  const uint8_t *ct, uint32_t ctlen,
                  const uint8_t tag[16], uint8_t *pt);

/* ---- AES-256-GCM (12-byte IV, 16-byte tag) ----------------------------- */
void aes256_gcm_seal(const uint8_t key[32], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *pt, uint32_t ptlen,
                     uint8_t *ct, uint8_t tag[16]);
int  aes256_gcm_open(const uint8_t key[32], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *ct, uint32_t ctlen,
                     const uint8_t tag[16], uint8_t *pt);

/* ---- X25519 (Curve25519 ECDH) ------------------------------------------ */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
extern const uint8_t x25519_basepoint[32];

/* ---- NIST P-256 (secp256r1 ECDH) --------------------------------------- */
void p256_pub_from_priv(const uint8_t priv[32], uint8_t pub[65]);  /* pub = 0x04||X||Y */
void p256_clamp_priv(uint8_t priv[32]);                            /* reduce into [1,n-1] */
int  p256_ecdh(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[65]);
