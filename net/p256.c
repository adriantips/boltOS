/* ===========================================================================
 *  NIST P-256 (secp256r1) ECDHE for the TLS stack.
 *
 *  Google/YouTube edges frequently pick secp256r1 for the ephemeral key
 *  exchange; without it the handshake dies. Pure integer arithmetic (the
 *  kernel has no FPU) on 256-bit values held as eight little-endian 32-bit
 *  limbs. Field reduction is plain binary long division by p -- not the
 *  fastest, but obviously correct, and a TLS handshake only needs two scalar
 *  multiplications. Scalar mult uses Jacobian coordinates (one inversion at
 *  the end). Curve: y^2 = x^3 - 3x + b over GF(p).
 * ===========================================================================*/
#include <stdint.h>
#include "string.h"

/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1, little-endian 32-bit limbs */
static const uint32_t P[8] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0xFFFFFFFF
};
/* curve b */
static const uint32_t B[8] = {
    0x27D2604B, 0x3BCE3C3E, 0xCC53B0F6, 0x651D06B0,
    0x769886BC, 0xB3EBBD55, 0xAA3A93E7, 0x5AC635D8
};
/* base point G */
static const uint32_t GX[8] = {
    0xD898C296, 0xF4A13945, 0x2DEB33A0, 0x77037D81,
    0x63A440F2, 0xF8BCE6E5, 0xE12C4247, 0x6B17D1F2
};
static const uint32_t GY[8] = {
    0x37BF51F5, 0xCBB64068, 0x6B315ECE, 0x2BCE3357,
    0x7C0F9E16, 0x8EE7EB4A, 0xFE1A7F9B, 0x4FE342E2
};
/* group order n (for clamping the private scalar) */
static const uint32_t N[8] = {
    0xFC632551, 0xF3B9CAC2, 0xA7179E84, 0xBCE6FAAD,
    0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF
};

/* --------------------------- big-int helpers --------------------------- */
static int bn_cmp(const uint32_t *a, const uint32_t *b) {        /* 8 limbs */
    for (int i = 7; i >= 0; i--) { if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1; }
    return 0;
}
static uint32_t bn_add(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint64_t c = 0;
    for (int i = 0; i < 8; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)c;
}
static uint32_t bn_sub(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint64_t br = 0;
    for (int i = 0; i < 8; i++) { uint64_t t = (uint64_t)a[i] - b[i] - br; r[i] = (uint32_t)t; br = (t >> 63) & 1; }
    return (uint32_t)br;
}
static int bn_is_zero(const uint32_t *a) { uint32_t x = 0; for (int i = 0; i < 8; i++) x |= a[i]; return x == 0; }

/* --------------------------- field arithmetic -------------------------- */
static void f_add(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t c = bn_add(r, a, b);
    if (c || bn_cmp(r, P) >= 0) bn_sub(r, r, P);
}
static void f_sub(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t br = bn_sub(r, a, b);
    if (br) bn_add(r, r, P);                       /* add p back if it went negative */
}
/* r = (a * b) mod p, via 512-bit product + binary long division remainder */
static void f_mul(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t prod[16];
    for (int i = 0; i < 16; i++) prod[i] = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)t; carry = t >> 32;
        }
        prod[i + 8] += (uint32_t)carry;
    }
    /* remainder = prod mod P, processing bits MSB->LSB */
    uint32_t rem[8]; for (int i = 0; i < 8; i++) rem[i] = 0;
    for (int bit = 511; bit >= 0; bit--) {
        /* rem <<= 1 */
        uint32_t carry = (prod[bit >> 5] >> (bit & 31)) & 1;
        for (int i = 0; i < 8; i++) { uint32_t nc = rem[i] >> 31; rem[i] = (rem[i] << 1) | carry; carry = nc; }
        if (carry || bn_cmp(rem, P) >= 0) bn_sub(rem, rem, P);
    }
    for (int i = 0; i < 8; i++) r[i] = rem[i];
}
static void f_sqr(uint32_t *r, const uint32_t *a) { f_mul(r, a, a); }
static void f_cpy(uint32_t *r, const uint32_t *a) { for (int i = 0; i < 8; i++) r[i] = a[i]; }
static void f_set(uint32_t *r, uint32_t v) { r[0] = v; for (int i = 1; i < 8; i++) r[i] = 0; }

/* r = a^-1 mod p  (Fermat: a^(p-2)); p-2 = ...FFFFFFFD */
static void f_inv(uint32_t *r, const uint32_t *a) {
    uint32_t e[8]; bn_sub(e, P, (const uint32_t[8]){2,0,0,0,0,0,0,0});  /* e = p-2 */
    uint32_t res[8]; f_set(res, 1);
    uint32_t base[8]; f_cpy(base, a);
    for (int bit = 0; bit < 256; bit++) {
        if ((e[bit >> 5] >> (bit & 31)) & 1) f_mul(res, res, base);
        f_sqr(base, base);
    }
    f_cpy(r, res);
}

/* --------------------------- EC point (Jacobian) ----------------------- */
typedef struct { uint32_t X[8], Y[8], Z[8]; } jpt;

static int j_is_inf(const jpt *p) { return bn_is_zero(p->Z); }

static void j_double(jpt *r, const jpt *p) {
    if (j_is_inf(p) || bn_is_zero(p->Y)) { f_set(r->X,1); f_set(r->Y,1); f_set(r->Z,0); return; }
    uint32_t A[8],Bv[8],C[8],D[8],t1[8],t2[8];
    f_sqr(A, p->Y);                       /* A = Y^2 */
    f_mul(Bv, p->X, A); f_add(Bv,Bv,Bv); f_add(Bv,Bv,Bv);  /* B = 4*X*Y^2 */
    f_sqr(C, A); f_add(C,C,C); f_add(C,C,C); f_add(C,C,C);  /* C = 8*Y^4 */
    /* D = 3*(X-Z^2)*(X+Z^2)  (since a=-3) */
    uint32_t z2[8]; f_sqr(z2, p->Z);
    f_sub(t1, p->X, z2); f_add(t2, p->X, z2); f_mul(t1, t1, t2);
    f_add(D, t1, t1); f_add(D, D, t1);    /* D = 3*t1 */
    f_sqr(r->X, D); f_sub(r->X, r->X, Bv); f_sub(r->X, r->X, Bv);  /* X' = D^2 - 2B */
    f_sub(t1, Bv, r->X); f_mul(t1, D, t1); f_sub(r->Y, t1, C);     /* Y' = D*(B-X')-C */
    f_mul(r->Z, p->Y, p->Z); f_add(r->Z, r->Z, r->Z);             /* Z' = 2*Y*Z */
}

/* r = p + q, with q in affine (qZ=1). Handles p==inf and equal points. */
static void j_add_affine(jpt *r, const jpt *p, const uint32_t *qx, const uint32_t *qy) {
    if (j_is_inf(p)) { f_cpy(r->X,qx); f_cpy(r->Y,qy); f_set(r->Z,1); return; }
    uint32_t z2[8],u2[8],s2[8],H[8],Rr[8],t1[8],t2[8];
    f_sqr(z2, p->Z);
    f_mul(u2, qx, z2);                    /* U2 = qx*Z^2 */
    f_mul(s2, qy, z2); f_mul(s2, s2, p->Z);  /* S2 = qy*Z^3 */
    f_sub(H, u2, p->X);                   /* H = U2 - X1 */
    f_sub(Rr, s2, p->Y);                  /* R = S2 - Y1 */
    if (bn_is_zero(H)) {
        if (bn_is_zero(Rr)) { jpt tmp; f_cpy(tmp.X,p->X); f_cpy(tmp.Y,p->Y); f_cpy(tmp.Z,p->Z); j_double(r,&tmp); return; }
        f_set(r->X,1); f_set(r->Y,1); f_set(r->Z,0); return;   /* infinity */
    }
    uint32_t H2[8],H3[8];
    f_sqr(H2, H); f_mul(H3, H2, H);
    f_mul(t1, p->X, H2);                  /* X1*H^2 */
    f_sqr(r->X, Rr); f_sub(r->X, r->X, H3); f_sub(r->X, r->X, t1); f_sub(r->X, r->X, t1); /* R^2 - H^3 - 2*X1*H^2 */
    f_sub(t2, t1, r->X); f_mul(t2, Rr, t2);
    f_mul(t1, p->Y, H3); f_sub(r->Y, t2, t1);     /* R*(X1*H^2 - X') - Y1*H^3 */
    f_mul(r->Z, p->Z, H);                          /* Z' = Z1*H */
}

/* scalar (big-endian 32 bytes) * (gx,gy) -> affine (rx,ry). returns 0 ok */
static int scalar_mul(uint32_t *rx, uint32_t *ry, const uint8_t k[32],
                      const uint32_t *gx, const uint32_t *gy) {
    jpt acc; f_set(acc.X,1); f_set(acc.Y,1); f_set(acc.Z,0);  /* infinity */
    for (int i = 0; i < 256; i++) {                 /* MSB first */
        jpt t; j_double(&t, &acc); acc = t;
        int byte = i >> 3, bit = 7 - (i & 7);
        if ((k[byte] >> bit) & 1) { jpt t2; j_add_affine(&t2, &acc, gx, gy); acc = t2; }
    }
    if (j_is_inf(&acc)) return -1;
    uint32_t zi[8], zi2[8], zi3[8];
    f_inv(zi, acc.Z); f_sqr(zi2, zi); f_mul(zi3, zi2, zi);
    f_mul(rx, acc.X, zi2);
    f_mul(ry, acc.Y, zi3);
    return 0;
}

/* --------------------------- byte (de)serialisation -------------------- */
static void be_to_limbs(uint32_t out[8], const uint8_t in[32]) {
    for (int i = 0; i < 8; i++) {
        const uint8_t *p = in + (7 - i) * 4;
        out[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
}
static void limbs_to_be(uint8_t out[32], const uint32_t in[8]) {
    for (int i = 0; i < 8; i++) {
        uint8_t *p = out + (7 - i) * 4;
        p[0] = in[i] >> 24; p[1] = in[i] >> 16; p[2] = in[i] >> 8; p[3] = (uint8_t)in[i];
    }
}

/* --------------------------- mod-n arithmetic -------------------------- */
/* The group order n is prime; used for ECDSA verification. Reduction is the
 * same obviously-correct binary long division as f_mul, but modulo N. */
static void n_reduce(uint32_t r[8], const uint32_t prod[16]) {
    uint32_t rem[8]; for (int i = 0; i < 8; i++) rem[i] = 0;
    for (int bit = 511; bit >= 0; bit--) {
        uint32_t carry = (prod[bit >> 5] >> (bit & 31)) & 1;
        for (int i = 0; i < 8; i++) { uint32_t nc = rem[i] >> 31; rem[i] = (rem[i] << 1) | carry; carry = nc; }
        if (carry || bn_cmp(rem, N) >= 0) bn_sub(rem, rem, N);
    }
    for (int i = 0; i < 8; i++) r[i] = rem[i];
}
static void n_mul(uint32_t *r, const uint32_t *a, const uint32_t *b) {
    uint32_t prod[16]; for (int i = 0; i < 16; i++) prod[i] = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < 8; j++) {
            uint64_t t = (uint64_t)a[i] * b[j] + prod[i + j] + carry;
            prod[i + j] = (uint32_t)t; carry = t >> 32;
        }
        prod[i + 8] += (uint32_t)carry;
    }
    n_reduce(r, prod);
}
static void n_mod(uint32_t r[8], const uint32_t a[8]) {       /* a already < 2^256 */
    uint32_t prod[16]; for (int i = 0; i < 16; i++) prod[i] = (i < 8) ? a[i] : 0;
    n_reduce(r, prod);
}
/* r = a^-1 mod n via Fermat (a^(n-2)) */
static void n_inv(uint32_t *r, const uint32_t *a) {
    uint32_t e[8]; bn_sub(e, N, (const uint32_t[8]){2,0,0,0,0,0,0,0});
    uint32_t res[8]; f_set(res, 1);
    uint32_t base[8]; for (int i = 0; i < 8; i++) base[i] = a[i];
    for (int bit = 0; bit < 256; bit++) {
        if ((e[bit >> 5] >> (bit & 31)) & 1) n_mul(res, res, base);
        n_mul(base, base, base);
    }
    for (int i = 0; i < 8; i++) r[i] = res[i];
}

/* ECDSA verify over P-256. pub = 0x04||X||Y (65 bytes); hash is the message
 * digest (leftmost 32 bytes used); r,s are 32-byte big-endian. Returns 0 if the
 * signature is valid, -1 otherwise. */
int p256_ecdsa_verify(const uint8_t *hash, uint32_t hlen,
                      const uint8_t pub[65], const uint8_t r[32], const uint8_t s[32]) {
    if (pub[0] != 0x04) return -1;
    uint32_t rr[8], ss[8];
    be_to_limbs(rr, r); be_to_limbs(ss, s);
    if (bn_is_zero(rr) || bn_is_zero(ss) || bn_cmp(rr, N) >= 0 || bn_cmp(ss, N) >= 0) return -1;

    /* z = leftmost min(hlen,32) bytes of the hash, big-endian, reduced mod n */
    uint8_t zb[32]; for (int i = 0; i < 32; i++) zb[i] = 0;
    uint32_t n = hlen < 32 ? hlen : 32;
    memcpy(zb, hash, n);                  /* take the leftmost bytes */
    uint32_t z[8]; be_to_limbs(z, zb); n_mod(z, z);

    uint32_t w[8]; n_inv(w, ss);
    uint32_t u1[8], u2[8]; n_mul(u1, z, w); n_mul(u2, rr, w);

    uint8_t u1b[32], u2b[32]; limbs_to_be(u1b, u1); limbs_to_be(u2b, u2);
    uint32_t qx[8], qy[8]; be_to_limbs(qx, pub + 1); be_to_limbs(qy, pub + 33);

    /* P = u1*G + u2*Q */
    uint32_t p1x[8], p1y[8], p2x[8], p2y[8];
    int inf1 = scalar_mul(p1x, p1y, u1b, GX, GY);   /* 0 ok, -1 = infinity */
    int inf2 = scalar_mul(p2x, p2y, u2b, qx, qy);
    uint32_t Rx[8];
    if (inf1 != 0 && inf2 != 0) return -1;
    else if (inf1 != 0) { for (int i = 0; i < 8; i++) Rx[i] = p2x[i]; }
    else if (inf2 != 0) { for (int i = 0; i < 8; i++) Rx[i] = p1x[i]; }
    else {
        jpt J; f_cpy(J.X, p1x); f_cpy(J.Y, p1y); f_set(J.Z, 1);
        jpt R; j_add_affine(&R, &J, p2x, p2y);
        if (j_is_inf(&R)) return -1;
        uint32_t zi[8], zi2[8]; f_inv(zi, R.Z); f_sqr(zi2, zi);
        f_mul(Rx, R.X, zi2);                          /* affine x */
    }
    n_mod(Rx, Rx);                                    /* x mod n */
    return bn_cmp(Rx, rr) == 0 ? 0 : -1;
}

/* ------------------------------ public API ----------------------------- */
/* priv must be a valid scalar in [1, n-1]; the caller supplies randomness and
 * we clamp it into range. pub is the uncompressed point 0x04||X||Y (65 bytes). */
void p256_pub_from_priv(const uint8_t priv[32], uint8_t pub[65]) {
    uint32_t rx[8], ry[8];
    scalar_mul(rx, ry, priv, GX, GY);
    pub[0] = 0x04;
    limbs_to_be(pub + 1, rx);
    limbs_to_be(pub + 33, ry);
}

/* Reduce a 32-byte big-endian value into [1, n-1] for use as a private key. */
void p256_clamp_priv(uint8_t priv[32]) {
    uint32_t d[8]; be_to_limbs(d, priv);
    while (bn_cmp(d, N) >= 0) bn_sub(d, d, N);
    if (bn_is_zero(d)) d[0] = 1;
    limbs_to_be(priv, d);
}

/* ECDH: out = X-coordinate of priv * peer_pub. peer_pub is 0x04||X||Y.
 * Returns 0 on success, -1 if peer point is malformed/not on curve. */
int p256_ecdh(uint8_t out[32], const uint8_t priv[32], const uint8_t peer_pub[65]) {
    if (peer_pub[0] != 0x04) return -1;
    uint32_t qx[8], qy[8];
    be_to_limbs(qx, peer_pub + 1);
    be_to_limbs(qy, peer_pub + 33);
    /* verify the point is on the curve: y^2 == x^3 - 3x + b */
    uint32_t y2[8], x3[8], t[8], three_x[8];
    f_sqr(y2, qy);
    f_sqr(t, qx); f_mul(x3, t, qx);
    f_add(three_x, qx, qx); f_add(three_x, three_x, qx);
    f_sub(x3, x3, three_x); f_add(x3, x3, B);
    if (bn_cmp(y2, x3) != 0) return -1;
    uint32_t rx[8], ry[8];
    if (scalar_mul(rx, ry, priv, qx, qy) != 0) return -1;
    limbs_to_be(out, rx);
    return 0;
}
