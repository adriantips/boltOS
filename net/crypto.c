#include "crypto.h"
#include "string.h"

/* ===========================================================================
 *  Crypto primitives for the TLS 1.2 client. Compact, standalone reference
 *  implementations:
 *    - SHA-256          (FIPS 180-4)
 *    - HMAC-SHA256      (RFC 2104)
 *    - AES-128          (FIPS 197, forward cipher)
 *    - AES-128-GCM      (NIST SP 800-38D)
 *    - X25519           (RFC 7748; the field arithmetic follows TweetNaCl,
 *                        which is in the public domain)
 * ===========================================================================*/

/* =========================== SHA-256 ===================================== */
static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void sha256_init(sha256_ctx *c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->total = 0; c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while (len) {
        uint32_t take = 64 - c->n; if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->n != 56) sha256_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8*i));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)(c->h[i]);
    }
}

void sha256(const void *data, uint32_t len, uint8_t out[32]) {
    sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* =========================== HMAC-SHA256 ================================= */
void hmac_sha256(const uint8_t *key, uint32_t klen,
                 const uint8_t *msg, uint32_t mlen, uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];
    memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k); else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }

    sha256_ctx c;
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

/* =========================== AES-128 ==================================== */
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

void aes128_init(aes128_ctx *c, const uint8_t key[16]) {
    uint8_t *rk = c->rk;
    memcpy(rk, key, 16);
    static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    for (int i = 1; i <= 10; i++) {
        uint8_t *prev = rk + (i-1)*16, *cur = rk + i*16;
        uint8_t t[4] = { prev[13], prev[14], prev[15], prev[12] };   /* rotword */
        for (int j = 0; j < 4; j++) t[j] = sbox[t[j]];
        t[0] ^= rcon[i-1];
        for (int j = 0; j < 4; j++) cur[j] = prev[j] ^ t[j];
        for (int j = 4; j < 16; j++) cur[j] = cur[j-4] ^ prev[j];
    }
}

void aes128_encrypt(const aes128_ctx *c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    const uint8_t *rk = c->rk;
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];           /* SubBytes */
        uint8_t t[16];                                            /* ShiftRows */
        static const uint8_t sr[16] = {0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11};
        for (int i = 0; i < 16; i++) t[i] = s[sr[i]];
        if (round != 10) {                                        /* MixColumns */
            for (int col = 0; col < 4; col++) {
                uint8_t *p = t + col*4;
                uint8_t a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = (uint8_t)(xtime(a0) ^ (xtime(a1)^a1) ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2)^a2) ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3)^a3));
                p[3] = (uint8_t)((xtime(a0)^a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        const uint8_t *krk = rk + round*16;                       /* AddRoundKey */
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ krk[i];
    }
    memcpy(out, s, 16);
}

/* =========================== AES-256 ==================================== */
/* 256-bit key -> 15 round keys (Nk=8, Nr=14). Same SubBytes/ShiftRows/
 * MixColumns round as AES-128; only the key schedule and round count differ. */
void aes256_init(aes256_ctx *c, const uint8_t key[32]) {
    uint8_t *rk = c->rk;
    memcpy(rk, key, 32);
    static const uint8_t rcon[7] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40};
    /* 60 words total; words come in groups of Nk=8. */
    for (int i = 8; i < 60; i++) {
        uint8_t t[4];
        uint8_t *w = rk + i*4, *wp = rk + (i-1)*4, *wk = rk + (i-8)*4;
        t[0]=wp[0]; t[1]=wp[1]; t[2]=wp[2]; t[3]=wp[3];
        if (i % 8 == 0) {
            uint8_t tmp = t[0]; t[0]=sbox[t[1]]; t[1]=sbox[t[2]]; t[2]=sbox[t[3]]; t[3]=sbox[tmp];
            t[0] ^= rcon[i/8 - 1];
        } else if (i % 8 == 4) {
            t[0]=sbox[t[0]]; t[1]=sbox[t[1]]; t[2]=sbox[t[2]]; t[3]=sbox[t[3]];
        }
        for (int j = 0; j < 4; j++) w[j] = wk[j] ^ t[j];
    }
}

void aes256_encrypt(const aes256_ctx *c, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    const uint8_t *rk = c->rk;
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 14; round++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        uint8_t t[16];
        static const uint8_t sr[16] = {0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11};
        for (int i = 0; i < 16; i++) t[i] = s[sr[i]];
        if (round != 14) {
            for (int col = 0; col < 4; col++) {
                uint8_t *p = t + col*4;
                uint8_t a0=p[0],a1=p[1],a2=p[2],a3=p[3];
                p[0] = (uint8_t)(xtime(a0) ^ (xtime(a1)^a1) ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2)^a2) ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3)^a3));
                p[3] = (uint8_t)((xtime(a0)^a0) ^ a1 ^ a2 ^ xtime(a3));
            }
        }
        const uint8_t *krk = rk + round*16;
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ krk[i];
    }
    memcpy(out, s, 16);
}

/* =========================== AES-128-GCM ================================ */
/* GF(2^128) multiply, blocks big-endian, reduction poly per SP 800-38D. */
static void gf_mul(uint8_t *x, const uint8_t *y) {
    uint8_t z[16] = {0}, v[16];
    memcpy(v, y, 16);
    for (int i = 0; i < 128; i++) {
        if ((x[i >> 3] >> (7 - (i & 7))) & 1)
            for (int j = 0; j < 16; j++) z[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (uint8_t)((v[j] >> 1) | ((v[j-1] & 1) << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}

static void ghash(const uint8_t H[16], const uint8_t *aad, uint32_t alen,
                  const uint8_t *ct, uint32_t clen, uint8_t out[16]) {
    uint8_t y[16] = {0};
    for (uint32_t off = 0; off < alen; off += 16) {
        uint32_t n = alen - off; if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) y[i] ^= aad[off+i];
        gf_mul(y, H);
    }
    for (uint32_t off = 0; off < clen; off += 16) {
        uint32_t n = clen - off; if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) y[i] ^= ct[off+i];
        gf_mul(y, H);
    }
    uint8_t lb[16] = {0};
    uint64_t abits = (uint64_t)alen * 8, cbits = (uint64_t)clen * 8;
    for (int i = 0; i < 8; i++) { lb[i] = (uint8_t)(abits >> (56-8*i)); lb[8+i] = (uint8_t)(cbits >> (56-8*i)); }
    for (int i = 0; i < 16; i++) y[i] ^= lb[i];
    gf_mul(y, H);
    memcpy(out, y, 16);
}

static void inc32(uint8_t ctr[16]) {
    for (int i = 15; i >= 12; i--) { if (++ctr[i]) break; }
}

/* GCM is cipher-agnostic: it only needs a 128-bit block encrypt. A function
 * pointer lets the same core drive AES-128 or AES-256 (and any future block
 * cipher). `ks` is the opaque key schedule passed straight back to `enc`. */
typedef void (*blk_enc)(const void *ks, const uint8_t in[16], uint8_t out[16]);
static void aes128_blk(const void *ks, const uint8_t in[16], uint8_t out[16]) {
    aes128_encrypt((const aes128_ctx *)ks, in, out);
}

/* CTR-mode keystream XOR starting from counter block icb (icb is advanced). */
static void gctr(blk_enc enc, const void *ks, uint8_t icb[16],
                 const uint8_t *in, uint32_t len, uint8_t *out) {
    uint8_t s[16];
    for (uint32_t off = 0; off < len; off += 16) {
        enc(ks, icb, s);
        inc32(icb);
        uint32_t n = len - off; if (n > 16) n = 16;
        for (uint32_t i = 0; i < n; i++) out[off+i] = in[off+i] ^ s[i];
    }
}

static void gcm_core(blk_enc enc, const void *ks, const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *in, uint32_t len, uint8_t *out, uint8_t tag[16]) {
    uint8_t H[16] = {0}; enc(ks, H, H);

    uint8_t j0[16];
    memcpy(j0, iv, 12); j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;

    uint8_t ctr[16]; memcpy(ctr, j0, 16); inc32(ctr);
    gctr(enc, ks, ctr, in, len, out);

    /* tag = E(J0) XOR GHASH(H, AAD, C); C is the ciphertext (out) */
    uint8_t s[16]; ghash(H, aad, aadlen, out, len, s);
    uint8_t ej0[16]; enc(ks, j0, ej0);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ ej0[i];
}

static int gcm_open(blk_enc enc, const void *ks, const uint8_t iv[12],
                    const uint8_t *aad, uint32_t aadlen,
                    const uint8_t *ct, uint32_t ctlen,
                    const uint8_t tag[16], uint8_t *pt) {
    /* Recompute the tag over the ciphertext, then decrypt. GHASH is over C, so
     * deriving the tag before producing plaintext is fine and matches seal(). */
    uint8_t H[16] = {0}; enc(ks, H, H);
    uint8_t j0[16]; memcpy(j0, iv, 12); j0[12]=0; j0[13]=0; j0[14]=0; j0[15]=1;

    uint8_t s[16]; ghash(H, aad, aadlen, ct, ctlen, s);
    uint8_t ej0[16]; enc(ks, j0, ej0);
    uint8_t want[16];
    for (int i = 0; i < 16; i++) want[i] = s[i] ^ ej0[i];

    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= want[i] ^ tag[i];

    uint8_t ctr[16]; memcpy(ctr, j0, 16); inc32(ctr);
    gctr(enc, ks, ctr, ct, ctlen, pt);
    return diff ? -1 : 0;
}

void aes_gcm_seal(const uint8_t key[16], const uint8_t iv[12],
                  const uint8_t *aad, uint32_t aadlen,
                  const uint8_t *pt, uint32_t ptlen,
                  uint8_t *ct, uint8_t tag[16]) {
    aes128_ctx ks; aes128_init(&ks, key);
    gcm_core(aes128_blk, &ks, iv, aad, aadlen, pt, ptlen, ct, tag);
}

int aes_gcm_open(const uint8_t key[16], const uint8_t iv[12],
                 const uint8_t *aad, uint32_t aadlen,
                 const uint8_t *ct, uint32_t ctlen,
                 const uint8_t tag[16], uint8_t *pt) {
    aes128_ctx ks; aes128_init(&ks, key);
    return gcm_open(aes128_blk, &ks, iv, aad, aadlen, ct, ctlen, tag, pt);
}

static void aes256_blk(const void *ks, const uint8_t in[16], uint8_t out[16]) {
    aes256_encrypt((const aes256_ctx *)ks, in, out);
}
void aes256_gcm_seal(const uint8_t key[32], const uint8_t iv[12],
                     const uint8_t *aad, uint32_t aadlen,
                     const uint8_t *pt, uint32_t ptlen,
                     uint8_t *ct, uint8_t tag[16]) {
    aes256_ctx ks; aes256_init(&ks, key);
    gcm_core(aes256_blk, &ks, iv, aad, aadlen, pt, ptlen, ct, tag);
}
int aes256_gcm_open(const uint8_t key[32], const uint8_t iv[12],
                    const uint8_t *aad, uint32_t aadlen,
                    const uint8_t *ct, uint32_t ctlen,
                    const uint8_t tag[16], uint8_t *pt) {
    aes256_ctx ks; aes256_init(&ks, key);
    return gcm_open(aes256_blk, &ks, iv, aad, aadlen, ct, ctlen, tag, pt);
}

/* =========================== SHA-512 / SHA-384 ========================== */
static uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static const uint64_t K512[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };

static void sha512_block(sha512_ctx *c, const uint8_t *p) {
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint64_t)p[i*8]<<56)|((uint64_t)p[i*8+1]<<48)|((uint64_t)p[i*8+2]<<40)|
               ((uint64_t)p[i*8+3]<<32)|((uint64_t)p[i*8+4]<<24)|((uint64_t)p[i*8+5]<<16)|
               ((uint64_t)p[i*8+6]<<8)|(uint64_t)p[i*8+7];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i-15],1) ^ ror64(w[i-15],8) ^ (w[i-15] >> 7);
        uint64_t s1 = ror64(w[i-2],19) ^ ror64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e,14) ^ ror64(e,18) ^ ror64(e,41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a,28) ^ ror64(a,34) ^ ror64(a,39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

void sha384_init(sha512_ctx *c) {
    c->h[0]=0xcbbb9d5dc1059ed8ULL; c->h[1]=0x629a292a367cd507ULL;
    c->h[2]=0x9159015a3070dd17ULL; c->h[3]=0x152fecd8f70e5939ULL;
    c->h[4]=0x67332667ffc00b31ULL; c->h[5]=0x8eb44a8768581511ULL;
    c->h[6]=0xdb0c2e0d64f98fa7ULL; c->h[7]=0x47b5481dbefa4fa4ULL;
    c->total = 0; c->n = 0;
}

void sha512_update(sha512_ctx *c, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    while (len) {
        uint32_t take = 128 - c->n; if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take; p += take; len -= take;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}

/* SHA-384 digest (first 48 bytes of the SHA-512 state). */
void sha384_final(sha512_ctx *c, uint8_t out[48]) {
    uint64_t bits = c->total * 8;             /* < 2^64 bits for our messages */
    uint8_t pad = 0x80;
    sha512_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->n != 112) sha512_update(c, &z, 1);
    uint8_t lb[16] = {0};                     /* 128-bit length, high half zero */
    for (int i = 0; i < 8; i++) lb[8+i] = (uint8_t)(bits >> (56 - 8*i));
    sha512_update(c, lb, 16);
    for (int i = 0; i < 6; i++)               /* 6 words = 48 bytes */
        for (int j = 0; j < 8; j++) out[i*8+j] = (uint8_t)(c->h[i] >> (56 - 8*j));
}

void hmac_sha384(const uint8_t *key, uint32_t klen,
                 const uint8_t *msg, uint32_t mlen, uint8_t out[48]) {
    uint8_t k[128], ipad[128], opad[128], inner[48];
    memset(k, 0, 128);
    if (klen > 128) { sha512_ctx t; sha384_init(&t); sha512_update(&t, key, klen); sha384_final(&t, k); }
    else memcpy(k, key, klen);
    for (int i = 0; i < 128; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha512_ctx c;
    sha384_init(&c); sha512_update(&c, ipad, 128); sha512_update(&c, msg, mlen); sha384_final(&c, inner);
    sha384_init(&c); sha512_update(&c, opad, 128); sha512_update(&c, inner, 48); sha384_final(&c, out);
}

/* =========================== X25519 ===================================== */
/* Field arithmetic over GF(2^255-19), TweetNaCl style (public domain). */
typedef int64_t gf[16];

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2*i] + ((int64_t)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}
static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i+1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void sel25519(gf p, gf q, int b) {
    int64_t t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2*i] = t[i] & 0xff; o[2*i+1] = t[i] >> 8; }
}
static void A(gf o, const gf a, const gf b) { for (int i=0;i<16;i++) o[i]=a[i]+b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i=0;i<16;i++) o[i]=a[i]-b[i]; }
static void M(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void S(gf o, const gf a) { M(o, a, a); }
static void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static const gf _121665 = {0xDB41, 1};

const uint8_t x25519_basepoint[32] = {9};

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (scalar[31] & 127) | 64;   /* clamp */
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d);
        S(d, e); S(f, a); M(a, c, a); M(c, b, e);
        A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f);
        M(a, c, _121665); A(a, a, d); M(c, c, a); M(a, d, f);
        M(d, b, x); S(b, e); sel25519(a, b, r); sel25519(c, d, r);
    }
    /* result q = a * inv(c) */
    gf invc;
    inv25519(invc, c);
    M(a, a, invc);
    pack25519(out, a);
}
