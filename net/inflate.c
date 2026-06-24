#include <stdint.h>
#include "inflate.h"

/* ===========================================================================
 *  DEFLATE (RFC 1951) + gzip (RFC 1952) inflate.
 *
 *  Canonical-Huffman, bit-at-a-time decode (the "puff/tinf" approach): small,
 *  obviously correct, fast enough for web pages. Output is bounded by dcap so a
 *  hostile or truncated stream can't run past the destination buffer.
 * ===========================================================================*/

#define MAXBITS    15
#define MAXLCODES  286
#define MAXDCODES  30
#define MAXCODES   (MAXLCODES + MAXDCODES)
#define FIXLCODES  288

struct state {
    const uint8_t *in; uint32_t inlen, inpos;
    uint32_t bitbuf, bitcnt;
    uint8_t  *out; uint32_t outcap, outpos;
};

struct huff { int16_t count[MAXBITS + 1]; int16_t symbol[MAXCODES]; };

static int getbit(struct state *s) {
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) return -1;
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1; s->bitcnt--;
    return b;
}
static int getbits(struct state *s, int need) {
    int val = 0;
    for (int i = 0; i < need; i++) {
        int b = getbit(s); if (b < 0) return -1;
        val |= b << i;
    }
    return val;
}

/* decode one symbol using canonical-Huffman tables */
static int decode(struct state *s, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = getbit(s); if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count; first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* build canonical-Huffman decode tables from a list of code lengths */
static void build(struct huff *h, const uint8_t *lengths, int n) {
    for (int i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int16_t offs[MAXBITS + 1]; offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (int16_t)i;
}

static int emit(struct state *s, uint8_t byte) {
    if (s->outpos >= s->outcap) return -1;
    s->out[s->outpos++] = byte;
    return 0;
}

/* RFC 1951 length / distance base + extra-bit tables */
static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_block(struct state *s, const struct huff *lh, const struct huff *dh) {
    for (;;) {
        int sym = decode(s, lh);
        if (sym < 0) return -1;
        if (sym == 256) return 0;                  /* end of block */
        if (sym < 256) { if (emit(s, (uint8_t)sym) != 0) return -1; continue; }
        sym -= 257;
        if (sym >= 29) return -1;
        int len = lbase[sym]; int e = getbits(s, lext[sym]); if (e < 0) return -1; len += e;
        int dsym = decode(s, dh); if (dsym < 0 || dsym >= 30) return -1;
        int dist = dbase[dsym]; e = getbits(s, dext[dsym]); if (e < 0) return -1; dist += e;
        if ((uint32_t)dist > s->outpos) return -1; /* refers before output start */
        for (int i = 0; i < len; i++) {
            uint8_t b = s->out[s->outpos - dist];
            if (emit(s, b) != 0) return -1;
        }
    }
}

static int stored_block(struct state *s) {
    s->bitbuf = 0; s->bitcnt = 0;                  /* align to byte boundary */
    if (s->inpos + 4 > s->inlen) return -1;
    uint32_t len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
    s->inpos += 4;                                 /* skip LEN + ~LEN */
    if (s->inpos + len > s->inlen) return -1;
    for (uint32_t i = 0; i < len; i++) if (emit(s, s->in[s->inpos++]) != 0) return -1;
    return 0;
}

static int fixed_block(struct state *s) {
    static struct huff lh, dh; static int built = 0;
    if (!built) {
        uint8_t ll[FIXLCODES];
        for (int i = 0;   i < 144; i++) ll[i] = 8;
        for (int i = 144; i < 256; i++) ll[i] = 9;
        for (int i = 256; i < 280; i++) ll[i] = 7;
        for (int i = 280; i < 288; i++) ll[i] = 8;
        build(&lh, ll, FIXLCODES);
        uint8_t dl[30]; for (int i = 0; i < 30; i++) dl[i] = 5;
        build(&dh, dl, 30);
        built = 1;
    }
    return inflate_block(s, &lh, &dh);
}

static const uint8_t clc_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static int dynamic_block(struct state *s) {
    int hlit = getbits(s, 5); if (hlit < 0) return -1; hlit += 257;
    int hdist = getbits(s, 5); if (hdist < 0) return -1; hdist += 1;
    int hclen = getbits(s, 4); if (hclen < 0) return -1; hclen += 4;
    if (hlit > MAXLCODES || hdist > MAXDCODES) return -1;

    uint8_t cl[19]; for (int i = 0; i < 19; i++) cl[i] = 0;
    for (int i = 0; i < hclen; i++) { int v = getbits(s, 3); if (v < 0) return -1; cl[clc_order[i]] = (uint8_t)v; }
    struct huff clh; build(&clh, cl, 19);

    uint8_t lengths[MAXLCODES + MAXDCODES];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = decode(s, &clh); if (sym < 0) return -1;
        if (sym < 16) lengths[n++] = (uint8_t)sym;
        else if (sym == 16) {
            if (n == 0) return -1;
            int r = getbits(s, 2); if (r < 0) return -1; r += 3;
            uint8_t prev = lengths[n - 1];
            while (r-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {
            int r = getbits(s, 3); if (r < 0) return -1; r += 3;
            while (r-- && n < total) lengths[n++] = 0;
        } else {
            int r = getbits(s, 7); if (r < 0) return -1; r += 11;
            while (r-- && n < total) lengths[n++] = 0;
        }
    }
    if (n != total) return -1;

    struct huff lh, dh;
    build(&lh, lengths, hlit);
    build(&dh, lengths + hlit, hdist);
    return inflate_block(s, &lh, &dh);
}

int inflate_raw(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap, uint32_t *dlen) {
    struct state s = { src, slen, 0, 0, 0, dst, dcap, 0 };
    int last;
    do {
        int bf = getbit(&s); if (bf < 0) return -1;
        last = bf;
        int type = getbits(&s, 2); if (type < 0) return -1;
        int rc;
        if (type == 0)      rc = stored_block(&s);
        else if (type == 1) rc = fixed_block(&s);
        else if (type == 2) rc = dynamic_block(&s);
        else                return -1;             /* reserved */
        if (rc != 0) return -1;
    } while (!last);
    if (dlen) *dlen = s.outpos;
    return 0;
}

int gunzip(const uint8_t *src, uint32_t slen, uint8_t *dst, uint32_t dcap, uint32_t *dlen) {
    if (slen < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8) return -1;
    uint8_t flg = src[3];
    uint32_t p = 10;                               /* fixed header */
    if (flg & 0x04) {                              /* FEXTRA */
        if (p + 2 > slen) return -1;
        uint32_t xl = src[p] | (src[p + 1] << 8); p += 2 + xl;
    }
    if (flg & 0x08) { while (p < slen && src[p]) p++; p++; }   /* FNAME */
    if (flg & 0x10) { while (p < slen && src[p]) p++; p++; }   /* FCOMMENT */
    if (flg & 0x02) p += 2;                        /* FHCRC */
    if (p >= slen) return -1;
    /* deflate body runs to 8 bytes before the end (CRC32 + ISIZE) */
    uint32_t blen = slen - p - 8;
    return inflate_raw(src + p, blen, dst, dcap, dlen);
}
