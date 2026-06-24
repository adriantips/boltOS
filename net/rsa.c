#include <stdint.h>
#include "rsa.h"
#include "crypto.h"
#include "string.h"

/* ===========================================================================
 *  RSA public-key signature verification (PKCS#1 v1.5 and PSS / MGF1).
 *
 *  Pure integer bignum: values are little-endian arrays of 32-bit limbs.
 *  Modular reduction is plain binary long division -- obviously correct, and
 *  verification needs only ~34 modmuls for e=65537, so speed is a non-issue.
 *  Supports moduli up to 4096 bits (128 limbs).
 * ===========================================================================*/

#define MAXL 128

static int bn_cmp_n(const uint32_t *a, const uint32_t *b, int nl) {
    for (int i = nl - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
/* a -= b (nl limbs); returns borrow */
static uint32_t bn_sub_n(uint32_t *a, const uint32_t *b, int nl) {
    uint64_t br = 0;
    for (int i = 0; i < nl; i++) { uint64_t t = (uint64_t)a[i] - b[i] - br; a[i] = (uint32_t)t; br = (t >> 63) & 1; }
    return (uint32_t)br;
}
static void bytes_to_bn(uint32_t *out, int nl, const uint8_t *b, uint32_t blen) {
    for (int i = 0; i < nl; i++) out[i] = 0;
    for (uint32_t i = 0; i < blen; i++)
        out[i >> 2] |= (uint32_t)b[blen - 1 - i] << ((i & 3) * 8);
}
static void bn_to_bytes(uint8_t *out, uint32_t outlen, const uint32_t *a) {
    for (uint32_t i = 0; i < outlen; i++)
        out[outlen - 1 - i] = (uint8_t)(a[i >> 2] >> ((i & 3) * 8));
}

/* r[nl] = prod[2*nl] mod m[nl], binary long division (MSB first). */
static void bn_reduce(uint32_t *r, const uint32_t *prod, const uint32_t *m, int nl) {
    uint32_t rem[MAXL + 1]; for (int i = 0; i <= nl; i++) rem[i] = 0;
    int bits = nl * 32 * 2;
    for (int bit = bits - 1; bit >= 0; bit--) {
        uint32_t carry = (prod[bit >> 5] >> (bit & 31)) & 1;
        for (int i = 0; i < nl; i++) { uint32_t nc = rem[i] >> 31; rem[i] = (rem[i] << 1) | carry; carry = nc; }
        rem[nl] = (rem[nl] << 1) | carry;
        if (rem[nl] || bn_cmp_n(rem, m, nl) >= 0) {
            uint32_t br = bn_sub_n(rem, m, nl);
            rem[nl] -= br;
        }
    }
    for (int i = 0; i < nl; i++) r[i] = rem[i];
}
static void bn_modmul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, int nl) {
    uint32_t prod[2 * MAXL]; for (int i = 0; i < 2 * nl; i++) prod[i] = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < nl; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)t; carry = t >> 32;
        }
        prod[i + nl] += (uint32_t)carry;
    }
    bn_reduce(r, prod, m, nl);
}
/* r = base^e mod m, e is a big-endian byte string. base must be < m. */
static void bn_modexp(uint32_t *r, const uint32_t *base, const uint32_t *m, int nl,
                      const uint8_t *e, uint32_t elen) {
    uint32_t res[MAXL]; for (int i = 0; i < nl; i++) res[i] = 0; res[0] = 1;
    int started = 0;
    for (uint32_t i = 0; i < elen; i++) {
        for (int b = 7; b >= 0; b--) {
            if (started) bn_modmul(res, res, res, m, nl);
            if ((e[i] >> b) & 1) {
                if (!started) started = 1;          /* result is still 1; just multiply */
                bn_modmul(res, res, base, m, nl);
            }
        }
    }
    for (int i = 0; i < nl; i++) r[i] = res[i];
}

/* Compute em = sig^e mod n as a byte string of length nlen. Returns 0 on ok. */
static int rsa_pubop(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen,
                     const uint8_t *sig, uint32_t siglen, uint8_t *em) {
    int nl = (int)((nlen + 3) / 4);
    if (nl > MAXL || siglen > nlen) return -1;
    uint32_t N[MAXL], S[MAXL], R[MAXL];
    bytes_to_bn(N, nl, n, nlen);
    bytes_to_bn(S, nl, sig, siglen);
    if (bn_cmp_n(S, N, nl) >= 0) return -1;          /* sig must be < modulus */
    bn_modexp(R, S, N, nl, e, elen);
    bn_to_bytes(em, nlen, R);
    return 0;
}

static void do_hash(int h384, const uint8_t *m, uint32_t ml, uint8_t *out) {
    if (h384) { sha512_ctx s; sha384_init(&s); sha512_update(&s, m, ml); sha384_final(&s, out); }
    else      { sha256_ctx s; sha256_init(&s); sha256_update(&s, m, ml); sha256_final(&s, out); }
}

/* DigestInfo DER prefixes for PKCS#1 v1.5 */
static const uint8_t DI_SHA256[] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const uint8_t DI_SHA384[] = {
    0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };

int rsa_verify_pkcs1(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen,
                     const uint8_t *sig, uint32_t siglen,
                     const uint8_t *hash, uint32_t hashlen, int h384) {
    uint8_t em[512];
    if (nlen > sizeof em) return -1;
    if (rsa_pubop(n, nlen, e, elen, sig, siglen, em) != 0) return -1;

    const uint8_t *di = h384 ? DI_SHA384 : DI_SHA256;
    uint32_t dilen = 19, hlen = h384 ? 48 : 32;
    if (hashlen != hlen) return -1;
    uint32_t tlen = dilen + hlen;
    /* EM = 0x00 0x01 PS(0xFF..) 0x00 T,  with PS >= 8 bytes */
    if (nlen < tlen + 11) return -1;
    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    uint32_t i = 2;
    while (i < nlen - tlen - 1 && em[i] == 0xFF) i++;
    if (i < 10 || em[i] != 0x00) return -1;          /* need >=8 0xFF + a 0x00 */
    i++;
    if (i + tlen != nlen) return -1;
    if (memcmp(em + i, di, dilen) != 0) return -1;
    return memcmp(em + i + dilen, hash, hlen) == 0 ? 0 : -1;
}

/* MGF1 with the selected hash. */
static void mgf1(int h384, const uint8_t *seed, uint32_t seedlen, uint8_t *mask, uint32_t masklen) {
    uint32_t hlen = h384 ? 48 : 32;
    uint8_t buf[64]; uint32_t done = 0, ctr = 0;
    while (done < masklen) {
        uint8_t in[128 + 4]; memcpy(in, seed, seedlen);
        in[seedlen] = (uint8_t)(ctr >> 24); in[seedlen+1] = (uint8_t)(ctr >> 16);
        in[seedlen+2] = (uint8_t)(ctr >> 8); in[seedlen+3] = (uint8_t)ctr;
        do_hash(h384, in, seedlen + 4, buf);
        uint32_t take = masklen - done; if (take > hlen) take = hlen;
        for (uint32_t i = 0; i < take; i++) mask[done + i] ^= buf[i];
        done += take; ctr++;
    }
}

/* EMSA-PSS-VERIFY with salt length == hash length (TLS 1.3 convention). */
int rsa_verify_pss(const uint8_t *n, uint32_t nlen, const uint8_t *e, uint32_t elen,
                   const uint8_t *sig, uint32_t siglen,
                   const uint8_t *hash, uint32_t hashlen, int h384) {
    uint8_t em[512];
    if (nlen > sizeof em) return -1;
    if (rsa_pubop(n, nlen, e, elen, sig, siglen, em) != 0) return -1;

    uint32_t hlen = h384 ? 48 : 32;
    if (hashlen != hlen) return -1;
    uint32_t emLen = nlen;                            /* modBits is a multiple of 8 here */
    uint32_t sLen = hlen;
    if (emLen < hlen + sLen + 2) return -1;
    if (em[emLen - 1] != 0xBC) return -1;

    uint32_t dbLen = emLen - hlen - 1;
    const uint8_t *H = em + dbLen;
    /* DB = maskedDB XOR MGF1(H, dbLen) */
    uint8_t db[512]; memcpy(db, em, dbLen);
    mgf1(h384, H, hlen, db, dbLen);
    db[0] &= 0x7F;                                    /* clear leftmost bit (8*emLen-emBits=1) */

    /* DB = PS(0x00..) || 0x01 || salt */
    uint32_t i = 0;
    while (i < dbLen - sLen - 1 && db[i] == 0x00) i++;
    if (i != dbLen - sLen - 1 || db[i] != 0x01) return -1;
    const uint8_t *salt = db + dbLen - sLen;

    /* H' = Hash(0x00*8 || mHash || salt) */
    uint8_t mp[8 + 64 + 64]; memset(mp, 0, 8);
    memcpy(mp + 8, hash, hlen);
    memcpy(mp + 8 + hlen, salt, sLen);
    uint8_t hp[64]; do_hash(h384, mp, 8 + hlen + sLen, hp);
    return memcmp(hp, H, hlen) == 0 ? 0 : -1;
}
