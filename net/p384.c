/* ===========================================================================
 *  NIST P-384 (secp384r1) ECDSA signature verification.
 *
 *  Real-world TLS certificate chains (DigiCert ECC, etc.) use P-384 keys in
 *  their intermediates/roots, so verifying a leaf up to the anchor needs this
 *  curve too. Verify-only: no ECDH. Same obviously-correct binary long-division
 *  field arithmetic as p256.c, widened to twelve 32-bit limbs. a = -3.
 * ===========================================================================*/
#include <stdint.h>
#include "string.h"

#define NL 12

/* p = 2^384 - 2^128 - 2^96 + 2^32 - 1 (little-endian limbs) */
static const uint32_t P[NL] = {
    0xFFFFFFFF,0x00000000,0x00000000,0xFFFFFFFF,0xFFFFFFFE,0xFFFFFFFF,
    0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF };
static const uint32_t B[NL] = {
    0xD3EC2AEF,0x2A85C8ED,0x8A2ED19D,0xC656398D,0x5013875A,0x0314088F,
    0xFE814112,0x181D9C6E,0xE3F82D19,0x988E056B,0xE23EE7E4,0xB3312FA7 };
static const uint32_t GX[NL] = {
    0x72760AB7,0x3A545E38,0xBF55296C,0x5502F25D,0x82542A38,0x59F741E0,
    0x8BA79B98,0x6E1D3B62,0xF320AD74,0x8EB1C71E,0xBE8B0537,0xAA87CA22 };
static const uint32_t GY[NL] = {
    0x90EA0E5F,0x7A431D7C,0x1D7E819D,0x0A60B1CE,0xB5F0B8C0,0xE9DA3113,
    0x289A147C,0xF8F41DBD,0x9292DC29,0x5D9E98BF,0x96262C6F,0x3617DE4A };
static const uint32_t N[NL] = {
    0xCCC52973,0xECEC196A,0x48B0A77A,0x581A0DB2,0xF4372DDF,0xC7634D81,
    0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF };

static int bn_cmp(const uint32_t *a, const uint32_t *b) {
    for (int i = NL - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint64_t c = 0; for (int i = 0; i < NL; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)c;
}
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint64_t br = 0; for (int i = 0; i < NL; i++) { uint64_t t = (uint64_t)a[i] - b[i] - br; r[i] = (uint32_t)t; br = (t >> 63) & 1; }
    return (uint32_t)br;
}
static int bn_is_zero(const uint32_t *a) { uint32_t x = 0; for (int i = 0; i < NL; i++) x |= a[i]; return x == 0; }

/* reduce a 2*NL-limb product modulo the NL-limb modulus m (MSB->LSB) */
static void reduce(uint32_t *r, const uint32_t *prod, const uint32_t *m) {
    uint32_t rem[NL]; for (int i = 0; i < NL; i++) rem[i] = 0;
    for (int bit = NL * 32 * 2 - 1; bit >= 0; bit--) {
        uint32_t carry = (prod[bit >> 5] >> (bit & 31)) & 1;
        for (int i = 0; i < NL; i++) { uint32_t nc = rem[i] >> 31; rem[i] = (rem[i] << 1) | carry; carry = nc; }
        if (carry || bn_cmp(rem, m) >= 0) bn_sub(rem, rem, m);
    }
    for (int i = 0; i < NL; i++) r[i] = rem[i];
}
static void mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m) {
    uint32_t prod[2 * NL]; for (int i = 0; i < 2 * NL; i++) prod[i] = 0;
    for (int i = 0; i < NL; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < NL; j++) { uint64_t t = (uint64_t)a[i] * b[j] + prod[i + j] + carry; prod[i + j] = (uint32_t)t; carry = t >> 32; }
        prod[i + NL] += (uint32_t)carry;
    }
    reduce(r, prod, m);
}
static void f_mul(uint32_t *r, const uint32_t *a, const uint32_t *b) { mulmod(r, a, b, P); }
static void f_sqr(uint32_t *r, const uint32_t *a) { mulmod(r, a, a, P); }
static void f_add(uint32_t *r, const uint32_t *a, const uint32_t *b) { uint32_t c = bn_add(r, a, b); if (c || bn_cmp(r, P) >= 0) bn_sub(r, r, P); }
static void f_sub(uint32_t *r, const uint32_t *a, const uint32_t *b) { if (bn_sub(r, a, b)) bn_add(r, r, P); }
static void f_cpy(uint32_t *r, const uint32_t *a) { for (int i = 0; i < NL; i++) r[i] = a[i]; }
static void f_set(uint32_t *r, uint32_t v) { r[0] = v; for (int i = 1; i < NL; i++) r[i] = 0; }

/* modular inverse via Fermat (exp = mod-2) */
static void inv_mod(uint32_t *r, const uint32_t *a, const uint32_t *m) {
    uint32_t two[NL]; f_set(two, 2); uint32_t e[NL]; bn_sub(e, m, two);
    uint32_t res[NL]; f_set(res, 1); uint32_t base[NL]; f_cpy(base, a);
    for (int bit = 0; bit < NL * 32; bit++) {
        if ((e[bit >> 5] >> (bit & 31)) & 1) mulmod(res, res, base, m);
        mulmod(base, base, base, m);
    }
    f_cpy(r, res);
}

typedef struct { uint32_t X[NL], Y[NL], Z[NL]; } jpt;
static int j_is_inf(const jpt *p) { return bn_is_zero(p->Z); }
static void j_double(jpt *r, const jpt *p) {
    if (j_is_inf(p) || bn_is_zero(p->Y)) { f_set(r->X,1); f_set(r->Y,1); f_set(r->Z,0); return; }
    uint32_t A[NL],Bv[NL],C[NL],D[NL],t1[NL],t2[NL],z2[NL];
    f_sqr(A, p->Y);
    f_mul(Bv, p->X, A); f_add(Bv,Bv,Bv); f_add(Bv,Bv,Bv);
    f_sqr(C, A); f_add(C,C,C); f_add(C,C,C); f_add(C,C,C);
    f_sqr(z2, p->Z);
    f_sub(t1, p->X, z2); f_add(t2, p->X, z2); f_mul(t1, t1, t2);
    f_add(D, t1, t1); f_add(D, D, t1);
    f_sqr(r->X, D); f_sub(r->X, r->X, Bv); f_sub(r->X, r->X, Bv);
    f_sub(t1, Bv, r->X); f_mul(t1, D, t1); f_sub(r->Y, t1, C);
    f_mul(r->Z, p->Y, p->Z); f_add(r->Z, r->Z, r->Z);
}
static void j_add_affine(jpt *r, const jpt *p, const uint32_t *qx, const uint32_t *qy) {
    if (j_is_inf(p)) { f_cpy(r->X,qx); f_cpy(r->Y,qy); f_set(r->Z,1); return; }
    uint32_t z2[NL],u2[NL],s2[NL],H[NL],Rr[NL],t1[NL],t2[NL],H2[NL],H3[NL];
    f_sqr(z2, p->Z);
    f_mul(u2, qx, z2);
    f_mul(s2, qy, z2); f_mul(s2, s2, p->Z);
    f_sub(H, u2, p->X);
    f_sub(Rr, s2, p->Y);
    if (bn_is_zero(H)) {
        if (bn_is_zero(Rr)) { jpt tmp = *p; j_double(r, &tmp); return; }
        f_set(r->X,1); f_set(r->Y,1); f_set(r->Z,0); return;
    }
    f_sqr(H2, H); f_mul(H3, H2, H);
    f_mul(t1, p->X, H2);
    f_sqr(r->X, Rr); f_sub(r->X, r->X, H3); f_sub(r->X, r->X, t1); f_sub(r->X, r->X, t1);
    f_sub(t2, t1, r->X); f_mul(t2, Rr, t2);
    f_mul(t1, p->Y, H3); f_sub(r->Y, t2, t1);
    f_mul(r->Z, p->Z, H);
}
/* k (big-endian, NL*4 bytes) * (gx,gy) -> affine; returns -1 if infinity */
static int scalar_mul(uint32_t *rx, uint32_t *ry, const uint8_t *k,
                      const uint32_t *gx, const uint32_t *gy) {
    jpt acc; f_set(acc.X,1); f_set(acc.Y,1); f_set(acc.Z,0);
    for (int i = 0; i < NL * 32; i++) {
        jpt t; j_double(&t, &acc); acc = t;
        int byte = i >> 3, bit = 7 - (i & 7);
        if ((k[byte] >> bit) & 1) { jpt t2; j_add_affine(&t2, &acc, gx, gy); acc = t2; }
    }
    if (j_is_inf(&acc)) return -1;
    uint32_t zi[NL], zi2[NL], zi3[NL];
    inv_mod(zi, acc.Z, P); f_sqr(zi2, zi); f_mul(zi3, zi2, zi);
    f_mul(rx, acc.X, zi2); f_mul(ry, acc.Y, zi3);
    return 0;
}
static void be_to_limbs(uint32_t *out, const uint8_t *in) {
    for (int i = 0; i < NL; i++) {
        const uint8_t *p = in + (NL - 1 - i) * 4;
        out[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
}
static void limbs_to_be(uint8_t *out, const uint32_t *in) {
    for (int i = 0; i < NL; i++) {
        uint8_t *p = out + (NL - 1 - i) * 4;
        p[0] = in[i] >> 24; p[1] = in[i] >> 16; p[2] = in[i] >> 8; p[3] = (uint8_t)in[i];
    }
}
static void nmod(uint32_t r[NL], const uint32_t a[NL]) {
    uint32_t prod[2 * NL]; for (int i = 0; i < 2 * NL; i++) prod[i] = (i < NL) ? a[i] : 0;
    reduce(r, prod, N);
}

/* ECDSA verify over P-384. pub = 0x04||X||Y (97 bytes), r/s 48-byte big-endian.
 * hash is the message digest (leftmost 48 bytes used). 0 = valid. */
int p384_ecdsa_verify(const uint8_t *hash, uint32_t hlen,
                      const uint8_t pub[97], const uint8_t *r, const uint8_t *s) {
    if (pub[0] != 0x04) return -1;
    uint32_t rr[NL], ss[NL];
    be_to_limbs(rr, r); be_to_limbs(ss, s);
    if (bn_is_zero(rr) || bn_is_zero(ss) || bn_cmp(rr, N) >= 0 || bn_cmp(ss, N) >= 0) return -1;

    uint8_t zb[48]; for (int i = 0; i < 48; i++) zb[i] = 0;
    uint32_t n = hlen < 48 ? hlen : 48;
    memcpy(zb, hash, n);
    uint32_t z[NL]; be_to_limbs(z, zb); nmod(z, z);

    uint32_t w[NL]; inv_mod(w, ss, N);
    uint32_t u1[NL], u2[NL]; mulmod(u1, z, w, N); mulmod(u2, rr, w, N);

    uint8_t u1b[48], u2b[48]; limbs_to_be(u1b, u1); limbs_to_be(u2b, u2);
    uint32_t qx[NL], qy[NL]; be_to_limbs(qx, pub + 1); be_to_limbs(qy, pub + 49);

    uint32_t p1x[NL], p1y[NL], p2x[NL], p2y[NL], Rx[NL];
    int inf1 = scalar_mul(p1x, p1y, u1b, GX, GY);
    int inf2 = scalar_mul(p2x, p2y, u2b, qx, qy);
    if (inf1 != 0 && inf2 != 0) return -1;
    else if (inf1 != 0) f_cpy(Rx, p2x);
    else if (inf2 != 0) f_cpy(Rx, p1x);
    else {
        jpt J; f_cpy(J.X, p1x); f_cpy(J.Y, p1y); f_set(J.Z, 1);
        jpt R; j_add_affine(&R, &J, p2x, p2y);
        if (j_is_inf(&R)) return -1;
        uint32_t zi[NL], zi2[NL]; inv_mod(zi, R.Z, P); f_sqr(zi2, zi);
        f_mul(Rx, R.X, zi2);
    }
    nmod(Rx, Rx);
    return bn_cmp(Rx, rr) == 0 ? 0 : -1;
}

/* keep B referenced (point-on-curve check is skipped for verify) */
const uint32_t *p384_b_unused(void) { return B; }
