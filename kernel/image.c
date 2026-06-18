/* ===========================================================================
 *  BoltOS  -  kernel/image.c
 *  Self-contained, integer-only image decoders for the browser:
 *      - PNG   (8/16-bit, colour types 0/2/3/4/6, non-interlaced) + DEFLATE
 *      - JPEG  (baseline sequential, Huffman, 4:4:4 / 4:2:2 / 4:2:0)
 *      - GIF   (87a/89a, first frame, LZW, transparency)
 *      - BMP   (24/32-bit uncompressed)
 *      - TGA   (truecolor 24/32 + grayscale 8, uncompressed or RLE)
 *  Output is a 0xAARRGGBB buffer. Large images are box-downscaled on the way
 *  out so the resident cost stays small on the 16 MiB kernel heap.
 * ===========================================================================*/
#include <stdint.h>
#include "image.h"
#include "kheap.h"
#include "string.h"

/* keep any single decoded image bounded; the browser scales to fit anyway */
#define IMG_MAX_PIXELS  1000000u   /* reject absurd headers before allocating */
#define IMG_FIT_DIM     640        /* downscale so max(w,h) <= this           */

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[0]<<24 | p[1]<<16 | p[2]<<8 | p[3]; }
static uint32_t rd_le32(const uint8_t *p) { return (uint32_t)p[3]<<24 | p[2]<<16 | p[1]<<8 | p[0]; }
static uint32_t rd_le16(const uint8_t *p) { return (uint32_t)p[1]<<8 | p[0]; }

static uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t)a<<24 | (uint32_t)r<<16 | (uint32_t)g<<8 | b;
}

/* ===========================================================================
 *  DEFLATE (RFC 1951) - adapted from Mark Adler's public-domain puff.c, with
 *  error returns instead of longjmp. Output buffer is pre-sized by the caller.
 * ===========================================================================*/
#define MAXBITS    15
#define MAXLCODES  286
#define MAXDCODES  30
#define MAXCODES   (MAXLCODES + MAXDCODES)
#define FIXLCODES  288

typedef struct {
    uint8_t       *out; uint32_t outlen, outcnt;
    const uint8_t *in;  uint32_t inlen, incnt;
    int            bitbuf, bitcnt, err;
} inflate_s;

typedef struct { short *count, *symbol; } huff_t;

static int inf_bits(inflate_s *s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) { s->err = 1; return 0; }
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int inf_stored(inflate_s *s) {
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return -1;
    unsigned len = s->in[s->incnt++]; len |= (unsigned)s->in[s->incnt++] << 8;
    s->incnt += 2;                              /* skip ones-complement length */
    if (s->incnt + len > s->inlen) return -1;
    while (len--) {
        if (s->outcnt == s->outlen) return -1;
        s->out[s->outcnt++] = s->in[s->incnt++];
    }
    return 0;
}

static int inf_construct(huff_t *h, const short *length, int n) {
    int len, left;
    short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = sym;
    return left;
}

static int inf_decode(inflate_s *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}

static const short LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short LEXT[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short DEXT[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inf_codes(inflate_s *s, const huff_t *lc, const huff_t *dc) {
    int sym;
    do {
        sym = inf_decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outcnt == s->outlen) return -1;
            s->out[s->outcnt++] = (uint8_t)sym;
        } else if (sym > 256) {
            sym -= 257; if (sym >= 29) return -1;
            int len = LBASE[sym] + inf_bits(s, LEXT[sym]);
            sym = inf_decode(s, dc); if (sym < 0) return -1;
            unsigned dist = DBASE[sym] + inf_bits(s, DEXT[sym]);
            if (s->err || dist > s->outcnt) return -1;
            if (s->outcnt + (unsigned)len > s->outlen) return -1;
            while (len--) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    } while (sym != 256);
    return 0;
}

static int inf_dynamic(inflate_s *s, huff_t *lc, huff_t *dc, short *lensym) {
    static const short ORDER[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    short lengths[MAXCODES];
    int nlen = inf_bits(s, 5) + 257;
    int ndist = inf_bits(s, 5) + 1;
    int ncode = inf_bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return -1;
    for (int i = 0; i < ncode; i++) lengths[ORDER[i]] = (short)inf_bits(s, 3);
    for (int i = ncode; i < 19; i++) lengths[ORDER[i]] = 0;

    short clcount[MAXBITS + 1], clsym[19];
    huff_t clcode = { clcount, clsym };
    if (inf_construct(&clcode, lengths, 19) != 0) return -1;

    int index = 0;
    while (index < nlen + ndist) {
        int sym = inf_decode(s, &clcode);
        if (sym < 0) return -1;
        if (sym < 16) lengths[index++] = (short)sym;
        else {
            int rep, val = 0;
            if (sym == 16) { if (index == 0) return -1; val = lengths[index - 1]; rep = 3 + inf_bits(s, 2); }
            else if (sym == 17) rep = 3 + inf_bits(s, 3);
            else rep = 11 + inf_bits(s, 7);
            if (index + rep > nlen + ndist) return -1;
            while (rep--) lengths[index++] = (short)val;
        }
    }
    if (s->err) return -1;
    if (inf_construct(lc, lengths, nlen) < 0) return -1;
    if (inf_construct(dc, lengths + nlen, ndist) < 0) return -1;
    (void)lensym;
    return inf_codes(s, lc, dc);
}

static int inf_fixed(inflate_s *s, huff_t *lc, huff_t *dc) {
    short lengths[FIXLCODES];
    int i;
    for (i = 0;   i < 144; i++) lengths[i] = 8;
    for (;        i < 256; i++) lengths[i] = 9;
    for (;        i < 280; i++) lengths[i] = 7;
    for (;        i < 288; i++) lengths[i] = 8;
    inf_construct(lc, lengths, FIXLCODES);
    for (i = 0; i < MAXDCODES; i++) lengths[i] = 5;
    inf_construct(dc, lengths, MAXDCODES);
    return inf_codes(s, lc, dc);
}

/* Inflate a raw DEFLATE stream into out[0..outcap); returns bytes produced or -1 */
static int inflate_raw(const uint8_t *src, uint32_t srclen, uint8_t *out, uint32_t outcap) {
    inflate_s s = { out, outcap, 0, src, srclen, 0, 0, 0, 0 };
    short lcount[MAXBITS + 1], lsym[FIXLCODES];
    short dcount[MAXBITS + 1], dsym[MAXDCODES];
    huff_t lc = { lcount, lsym }, dc = { dcount, dsym };
    int last, type;
    do {
        last = inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (s.err) return -1;
        int r;
        if (type == 0)      r = inf_stored(&s);
        else if (type == 1) r = inf_fixed(&s, &lc, &dc);
        else if (type == 2) r = inf_dynamic(&s, &lc, &dc, lsym);
        else                return -1;
        if (r != 0) return -1;
    } while (!last);
    return (int)s.outcnt;
}

/* ===========================================================================
 *  Downscale + finalise: box-average the buffer down until it fits IMG_FIT_DIM.
 * ===========================================================================*/
static image_t *finalize(uint32_t *px, int w, int h) {
    if (!px || w <= 0 || h <= 0) { if (px) kfree(px); return 0; }
    while (w > IMG_FIT_DIM || h > IMG_FIT_DIM) {
        int nw = (w + 1) / 2, nh = (h + 1) / 2;
        uint32_t *np = (uint32_t *)kmalloc((uint32_t)nw * nh * 4);
        if (!np) break;
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < nw; x++) {
                int x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
                uint32_t a = px[y0*w+x0], b = px[y0*w+x1], c = px[y1*w+x0], d = px[y1*w+x1];
                uint32_t A = ((a>>24&0xFF)+(b>>24&0xFF)+(c>>24&0xFF)+(d>>24&0xFF)) >> 2;
                uint32_t R = ((a>>16&0xFF)+(b>>16&0xFF)+(c>>16&0xFF)+(d>>16&0xFF)) >> 2;
                uint32_t G = ((a>>8 &0xFF)+(b>>8 &0xFF)+(c>>8 &0xFF)+(d>>8 &0xFF)) >> 2;
                uint32_t B = ((a    &0xFF)+(b    &0xFF)+(c    &0xFF)+(d    &0xFF)) >> 2;
                np[y*nw+x] = A<<24 | R<<16 | G<<8 | B;
            }
        }
        kfree(px); px = np; w = nw; h = nh;
    }
    image_t *im = (image_t *)kmalloc(sizeof(image_t));
    if (!im) { kfree(px); return 0; }
    im->w = w; im->h = h; im->px = px;
    return im;
}

/* ===========================================================================
 *  PNG
 * ===========================================================================*/
static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p>a?p-a:a-p, pb = p>b?p-b:b-p, pc = p>c?p-c:c-p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static image_t *decode_png(const uint8_t *d, uint32_t len) {
    if (len < 8) return 0;
    uint32_t w = 0, h = 0;
    int bitdepth = 0, colortype = 0, interlace = 0;
    uint8_t pal[256*3]; int npal = 0;
    uint8_t trns[256];  int ntrns = 0;

    /* size the concatenated IDAT buffer first */
    uint32_t idat_total = 0;
    for (uint32_t p = 8; p + 8 <= len; ) {
        uint32_t clen = rd_be32(d + p);
        const uint8_t *type = d + p + 4;
        if (p + 12 + clen > len) break;
        if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') idat_total += clen;
        if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') break;
        p += 12 + clen;
    }
    uint8_t *zbuf = idat_total ? (uint8_t *)kmalloc(idat_total) : 0;
    if (!zbuf) return 0;
    uint32_t zlen = 0;

    for (uint32_t p = 8; p + 8 <= len; ) {
        uint32_t clen = rd_be32(d + p);
        const uint8_t *type = d + p + 4, *body = d + p + 8;
        if (p + 12 + clen > len) break;
        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R' && clen >= 13) {
            w = rd_be32(body); h = rd_be32(body + 4);
            bitdepth = body[8]; colortype = body[9]; interlace = body[12];
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            npal = clen / 3; if (npal > 256) npal = 256;
            memcpy(pal, body, (uint32_t)npal * 3);
        } else if (type[0]=='t'&&type[1]=='R'&&type[2]=='N'&&type[3]=='S') {
            ntrns = clen > 256 ? 256 : clen; memcpy(trns, body, (uint32_t)ntrns);
        } else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
            memcpy(zbuf + zlen, body, clen); zlen += clen;
        } else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') break;
        p += 12 + clen;
    }

    if (!w || !h || interlace != 0 || (uint64_t)w*h > IMG_MAX_PIXELS) { kfree(zbuf); return 0; }
    if (bitdepth != 8 && bitdepth != 16) { kfree(zbuf); return 0; }

    int channels = colortype==0?1 : colortype==2?3 : colortype==3?1 : colortype==4?2 : colortype==6?4 : 0;
    if (!channels) { kfree(zbuf); return 0; }
    int sample = bitdepth == 16 ? 2 : 1;
    uint32_t bpp = (uint32_t)channels * sample;
    uint32_t stride = w * bpp;
    uint32_t rawsize = (stride + 1) * h;

    uint8_t *raw = (uint8_t *)kmalloc(rawsize);
    if (!raw) { kfree(zbuf); return 0; }
    /* PNG IDAT is a zlib stream: 2-byte header, then raw DEFLATE */
    int got = (zlen >= 2) ? inflate_raw(zbuf + 2, zlen - 2, raw, rawsize) : -1;
    kfree(zbuf);
    if (got < 0) { kfree(raw); return 0; }

    /* unfilter in place: produce contiguous scanlines (drop the filter byte) */
    uint8_t *lines = (uint8_t *)kmalloc(stride * h);
    if (!lines) { kfree(raw); return 0; }
    for (uint32_t y = 0; y < h; y++) {
        uint8_t f = raw[y * (stride + 1)];
        const uint8_t *src = raw + y * (stride + 1) + 1;
        uint8_t *cur = lines + y * stride;
        uint8_t *prev = y ? lines + (y - 1) * stride : 0;
        for (uint32_t x = 0; x < stride; x++) {
            int a = x >= bpp ? cur[x - bpp] : 0;
            int b = prev ? prev[x] : 0;
            int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
            int v = src[x];
            switch (f) {
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) >> 1; break;
                case 4: v += paeth(a, b, c); break;
                default: break;
            }
            cur[x] = (uint8_t)v;
        }
    }
    kfree(raw);

    uint32_t *px = (uint32_t *)kmalloc(w * h * 4);
    if (!px) { kfree(lines); return 0; }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = lines + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = row + x * bpp;
            uint8_t r, g, b, al = 255;
            if (colortype == 0)      { r = g = b = s[0]; }
            else if (colortype == 2) { r = s[0]; g = s[sample]; b = s[2*sample]; }
            else if (colortype == 3) { int idx = s[0]; r = pal[idx*3]; g = pal[idx*3+1]; b = pal[idx*3+2];
                                       if (idx < ntrns) al = trns[idx]; }
            else if (colortype == 4) { r = g = b = s[0]; al = s[sample]; }
            else                     { r = s[0]; g = s[sample]; b = s[2*sample]; al = s[3*sample]; }
            px[y*w+x] = argb(al, r, g, b);
        }
    }
    kfree(lines);
    return finalize(px, (int)w, (int)h);
}

/* ===========================================================================
 *  BMP (24/32-bit uncompressed)
 * ===========================================================================*/
static image_t *decode_bmp(const uint8_t *d, uint32_t len) {
    if (len < 54 || d[0] != 'B' || d[1] != 'M') return 0;
    uint32_t off = rd_le32(d + 10);
    uint32_t hdr = rd_le32(d + 14);
    int w = (int)rd_le32(d + 18);
    int h = (int)rd_le32(d + 22);
    int bpp = (int)rd_le16(d + 28);
    uint32_t comp = rd_le32(d + 30);
    (void)hdr;
    if (comp != 0 || (bpp != 24 && bpp != 32)) return 0;
    int flip = h > 0; if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || (uint64_t)w*h > IMG_MAX_PIXELS) return 0;
    int bytespp = bpp / 8;
    uint32_t rowsz = ((uint32_t)w * bytespp + 3) & ~3u;
    if (off + rowsz * (uint32_t)h > len) return 0;
    uint32_t *px = (uint32_t *)kmalloc((uint32_t)w * h * 4);
    if (!px) return 0;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = d + off + (uint32_t)(flip ? (h - 1 - y) : y) * rowsz;
        for (int x = 0; x < w; x++) {
            const uint8_t *s = row + x * bytespp;
            uint8_t al = bytespp == 4 ? s[3] : 255;
            px[y*w+x] = argb(al, s[2], s[1], s[0]);
        }
    }
    return finalize(px, w, h);
}

/* ===========================================================================
 *  TGA / Targa (uncompressed + RLE; truecolor 24/32 and grayscale 8)
 *  No magic number: only reached as the dispatch fallback, and every header
 *  field is range-checked so a non-TGA blob fails cleanly to NULL.
 * ===========================================================================*/
static image_t *decode_tga(const uint8_t *d, uint32_t len) {
    if (len < 18) return 0;
    uint8_t idlen   = d[0];
    uint8_t cmaptyp = d[1];
    uint8_t imgtyp  = d[2];
    int w   = (int)rd_le16(d + 12);
    int h   = (int)rd_le16(d + 14);
    int bpp = d[16];
    uint8_t desc = d[17];

    /* only non-colormapped truecolor/grayscale, raw (2/3) or RLE (10/11) */
    if (cmaptyp != 0) return 0;
    if (imgtyp != 2 && imgtyp != 3 && imgtyp != 10 && imgtyp != 11) return 0;
    int rle = (imgtyp == 10 || imgtyp == 11);
    if (bpp != 8 && bpp != 24 && bpp != 32) return 0;
    if (w <= 0 || h <= 0 || (uint64_t)w*h > IMG_MAX_PIXELS) return 0;

    int bytespp = bpp / 8;
    uint32_t pos = 18u + idlen;          /* skip image-ID (no color map here) */
    if (pos >= len) return 0;
    int top = (desc & 0x20) != 0;        /* descriptor bit5: top-left origin  */

    uint32_t *px = (uint32_t *)kmalloc((uint32_t)w * h * 4);
    if (!px) return 0;

    uint64_t total = (uint64_t)w * h, done = 0;
    uint8_t pixbuf[4];
    while (done < total) {
        int count, raw;
        if (rle) {
            if (pos >= len) { kfree(px); return 0; }
            uint8_t pkt = d[pos++];
            count = (pkt & 0x7F) + 1;
            raw   = !(pkt & 0x80);       /* high bit set => run packet */
        } else { count = (int)(total - done); raw = 1; }
        if ((uint64_t)count > total - done) count = (int)(total - done);

        for (int i = 0; i < count; i++) {
            if (raw || i == 0) {         /* run packet: read one pixel, repeat */
                if (pos + (uint32_t)bytespp > len) { kfree(px); return 0; }
                for (int b = 0; b < bytespp; b++) pixbuf[b] = d[pos++];
            }
            uint32_t argbpx;
            if (bytespp == 1) argbpx = argb(255, pixbuf[0], pixbuf[0], pixbuf[0]);
            else argbpx = argb(bytespp == 4 ? pixbuf[3] : 255,
                               pixbuf[2], pixbuf[1], pixbuf[0]);
            uint64_t idx = done + i;
            int x = (int)(idx % (uint64_t)w);
            int y = (int)(idx / (uint64_t)w);
            int ry = top ? y : (h - 1 - y);
            px[ry*w + x] = argbpx;
        }
        done += count;
    }
    return finalize(px, w, h);
}

/* ===========================================================================
 *  GIF (first frame, LZW)
 * ===========================================================================*/
static image_t *decode_gif(const uint8_t *d, uint32_t len) {
    if (len < 13 || d[0]!='G'||d[1]!='I'||d[2]!='F') return 0;
    int w = (int)rd_le16(d + 6), h = (int)rd_le16(d + 8);
    if (w <= 0 || h <= 0 || (uint64_t)w*h > IMG_MAX_PIXELS) return 0;
    uint8_t flags = d[10];
    uint32_t p = 13;
    uint8_t gpal[256*3]; int gpaln = 0;
    if (flags & 0x80) { gpaln = 2 << (flags & 7); if (p + (uint32_t)gpaln*3 > len) return 0;
        memcpy(gpal, d + p, (uint32_t)gpaln*3); p += (uint32_t)gpaln*3; }

    int transparent = -1;
    /* walk blocks until the first image descriptor */
    while (p < len) {
        uint8_t b = d[p++];
        if (b == 0x21) {                       /* extension */
            uint8_t label = d[p++];
            if (label == 0xF9) {               /* graphic control */
                uint8_t bs = d[p];
                if (bs >= 4 && (d[p+1] & 1)) transparent = d[p+4];
            }
            while (p < len) { uint8_t sz = d[p++]; if (!sz) break; p += sz; }
        } else if (b == 0x2C) {                /* image descriptor */
            int iw = (int)rd_le16(d + p + 4), ih = (int)rd_le16(d + p + 6);
            uint8_t lf = d[p + 8]; p += 9;
            uint8_t lpal[256*3]; const uint8_t *pal = gpal; int paln = gpaln;
            if (lf & 0x80) { paln = 2 << (lf & 7); memcpy(lpal, d + p, (uint32_t)paln*3); pal = lpal; p += (uint32_t)paln*3; }
            int interlaced = lf & 0x40;
            int mincode = d[p++];
            if (mincode < 2 || mincode > 8) return 0;

            uint32_t *px = (uint32_t *)kmalloc((uint32_t)iw * ih * 4);
            uint8_t  *idx = (uint8_t *)kmalloc((uint32_t)iw * ih);
            if (!px || !idx) { if (px) kfree(px); if (idx) kfree(idx); return 0; }

            /* LZW decode. The bit stream is continuous across length-prefixed
             * sub-blocks, so refill one byte at a time tracking the sub-block. */
            int clear = 1 << mincode, end = clear + 1;
            int codesize = mincode + 1, next = end + 1, oldcode = -1;
            uint16_t pref[4096]; uint8_t suf[4096], stack[4096]; int sp = 0;
            uint32_t bitbuf = 0; int bits = 0; uint32_t outp = 0, total = (uint32_t)iw * ih;
            int firstbyte = 0, sub = 0;
            for (;;) {
                while (bits < codesize) {
                    if (sub == 0) { if (p >= len) goto lzw_done; sub = d[p++]; if (sub == 0) goto lzw_done; }
                    bitbuf |= (uint32_t)d[p++] << bits; bits += 8; sub--;
                    if (p > len) goto lzw_done;
                }
                int code = bitbuf & ((1 << codesize) - 1);
                bitbuf >>= codesize; bits -= codesize;
                if (code == clear) { codesize = mincode + 1; next = end + 1; oldcode = -1; continue; }
                if (code == end) break;
                if (oldcode == -1) { firstbyte = code; oldcode = code;
                    if (outp < total) idx[outp++] = (uint8_t)code; continue; }
                int in = code;
                if (code >= next) { stack[sp++] = (uint8_t)firstbyte; code = oldcode; }
                while (code >= clear) { if (sp >= 4096) goto lzw_done; stack[sp++] = suf[code]; code = pref[code]; }
                firstbyte = code; stack[sp++] = (uint8_t)code;
                while (sp > 0 && outp < total) idx[outp++] = stack[--sp];
                sp = 0;
                if (next < 4096) { pref[next] = (uint16_t)oldcode; suf[next] = (uint8_t)firstbyte; next++;
                    if (next == (1 << codesize) && codesize < 12) codesize++; }
                oldcode = in;
            }
        lzw_done:
            /* skip any trailing sub-blocks */
            while (p < len) { uint8_t sz = d[p++]; if (!sz) break; p += sz; }

            /* map source scanlines to destination rows (de-interlace if needed) */
            int *rowmap = 0;
            if (interlaced) {
                rowmap = (int *)kmalloc((uint32_t)ih * sizeof(int));
                if (rowmap) { int oi = 0;
                    for (int y = 0; y < ih; y += 8) rowmap[oi++] = y;
                    for (int y = 4; y < ih; y += 8) rowmap[oi++] = y;
                    for (int y = 2; y < ih; y += 8) rowmap[oi++] = y;
                    for (int y = 1; y < ih; y += 8) rowmap[oi++] = y;
                }
            }
            for (int y = 0; y < ih; y++) {
                int dy = (interlaced && rowmap) ? rowmap[y] : y;
                for (int x = 0; x < iw; x++) {
                    int ci = idx[y*iw+x];
                    uint8_t al = (ci == transparent) ? 0 : 255;
                    uint8_t r = ci < paln ? pal[ci*3] : 0, g = ci < paln ? pal[ci*3+1] : 0, bl = ci < paln ? pal[ci*3+2] : 0;
                    px[dy*iw+x] = argb(al, r, g, bl);
                }
            }
            if (rowmap) kfree(rowmap);
            kfree(idx);
            return finalize(px, iw, ih);
        } else break;
    }
    return 0;
}

/* ===========================================================================
 *  JPEG (baseline sequential). Integer Huffman + IDCT (adapted from the
 *  public-domain stb_image), nearest-neighbour chroma upsampling.
 * ===========================================================================*/
#define JFAST_BITS 9
static const int JBMASK[17] = {0,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767,65535};
static const int JBIAS[17]  = {0,-1,-3,-7,-15,-31,-63,-127,-255,-511,-1023,-2047,-4095,-8191,-16383,-32767,-65535};
static const uint8_t JZZ[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,
    27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,
    44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

typedef struct {
    uint8_t  size[257], values[256];
    uint8_t  fast[1 << JFAST_BITS];
    uint16_t code[256];
    int      maxcode[18], delta[17];
} jhuff_t;

typedef struct {
    const uint8_t *d; uint32_t len, pos;
    int      code_bits; uint32_t code_buffer;
    int      marker, nomore;
    jhuff_t  huff_dc[4], huff_ac[4];
    uint16_t dequant[4][64];
    int      img_w, img_h, ncomp, restart;
    struct { int id, h, v, qt, dc, ac, dcpred; uint8_t *data; int w; } comp[4];
    int      hmax, vmax, mcu_w, mcu_h, mcus_x, mcus_y;
} jpg_t;

static int jbuild_huff(jhuff_t *h, const uint8_t *count) {
    int i, j, k = 0, code;
    for (i = 0; i < 16; i++) for (j = 0; j < count[i]; j++) h->size[k++] = (uint8_t)(i + 1);
    h->size[k] = 0;
    code = 0; k = 0;
    for (j = 1; j <= 16; j++) {
        h->delta[j] = k - code;
        if (h->size[k] == j) { while (h->size[k] == j) h->code[k++] = (uint16_t)code++; if (code - 1 >= (1 << j)) return 0; }
        h->maxcode[j] = code << (16 - j);
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    memset(h->fast, 255, sizeof(h->fast));
    for (i = 0; i < k; i++) {
        int s = h->size[i];
        if (s <= JFAST_BITS) {
            int c = h->code[i] << (JFAST_BITS - s), m = 1 << (JFAST_BITS - s);
            for (j = 0; j < m; j++) h->fast[c + j] = (uint8_t)i;
        }
    }
    return 1;
}

static uint8_t jget8(jpg_t *j) { return j->pos < j->len ? j->d[j->pos++] : 0; }
static int jget16(jpg_t *j) { int a = jget8(j); return (a << 8) | jget8(j); }

static void jgrow(jpg_t *j) {
    do {
        unsigned b = j->nomore ? 0 : jget8(j);
        if (b == 0xff) {
            int c = jget8(j);
            while (c == 0xff) c = jget8(j);
            if (c != 0) { j->marker = c; j->nomore = 1; return; }
        }
        j->code_buffer |= b << (24 - j->code_bits);
        j->code_bits += 8;
    } while (j->code_bits <= 24);
}

static int jhuff_decode(jpg_t *j, jhuff_t *h) {
    if (j->code_bits < 16) jgrow(j);
    int c = (j->code_buffer >> (32 - JFAST_BITS)) & ((1 << JFAST_BITS) - 1);
    int k = h->fast[c];
    if (k < 255) {
        int s = h->size[k];
        if (s > j->code_bits) return -1;
        j->code_buffer <<= s; j->code_bits -= s;
        return h->values[k];
    }
    unsigned temp = j->code_buffer >> 16;
    for (k = JFAST_BITS + 1; ; k++) if (temp < (unsigned)h->maxcode[k]) break;
    if (k == 17) { j->code_bits -= 16; return -1; }
    if (k > j->code_bits) return -1;
    c = ((j->code_buffer >> (32 - k)) & JBMASK[k]) + h->delta[k];
    j->code_bits -= k; j->code_buffer <<= k;
    return h->values[c];
}

static int jextend(jpg_t *j, int n) {
    if (n == 0) return 0;
    if (j->code_bits < n) jgrow(j);
    int sgn = (int32_t)j->code_buffer >> 31;
    unsigned k = (j->code_buffer << n) | (j->code_buffer >> (32 - n));
    j->code_buffer = k & ~JBMASK[n];
    k &= JBMASK[n];
    j->code_bits -= n;
    return (int)k + (JBIAS[n] & ~sgn);
}

static int jdecode_block(jpg_t *j, short data[64], jhuff_t *hdc, jhuff_t *hac, uint16_t *dq) {
    int t = jhuff_decode(j, hdc);
    if (t < 0) return 0;
    int diff = t ? jextend(j, t) : 0;
    memset(data, 0, 64 * sizeof(short));
    int k = 1;
    do {
        int rs = jhuff_decode(j, hac);
        if (rs < 0) return 0;
        int s = rs & 15, r = rs >> 4;
        if (s == 0) { if (rs != 0xf0) break; k += 16; }
        else { k += r; if (k >= 64) break; int zig = JZZ[k]; data[zig] = (short)(jextend(j, s) * dq[k]); k++; }
    } while (k < 64);
    /* DC stored by caller; return diff via data[0] convention */
    data[0] = (short)diff;             /* caller adds predictor and re-multiplies */
    return 1;
}

#define JF2F(x) ((int)((x) * 4096 + 0.5))     /* compile-time constant fold */
#define JFSH(x) ((x) * 4096)
#define JIDCT_1D(s0,s1,s2,s3,s4,s5,s6,s7) \
    int t0,t1,t2,t3,p1,p2,p3,p4,p5,x0,x1,x2,x3; \
    p2 = s2; p3 = s6; \
    p1 = (p2 + p3) * JF2F(0.5411961f); \
    t2 = p1 + p3 * JF2F(-1.847759065f); \
    t3 = p1 + p2 * JF2F(0.765366865f); \
    p2 = s0; p3 = s4; \
    t0 = JFSH(p2 + p3); t1 = JFSH(p2 - p3); \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * JF2F(1.175875602f); \
    t0 = t0 * JF2F(0.298631336f); t1 = t1 * JF2F(2.053119869f); \
    t2 = t2 * JF2F(3.072711026f); t3 = t3 * JF2F(1.501321110f); \
    p1 = p5 + p1 * JF2F(-0.899976223f); p2 = p5 + p2 * JF2F(-2.562915447f); \
    p3 = p3 * JF2F(-1.961570560f); p4 = p4 * JF2F(-0.390180644f); \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static void jidct(uint8_t *out, int stride, short data[64]) {
    int i, val[64], *v = val; short *dd = data;
    for (i = 0; i < 8; i++, dd++, v++) {
        if (dd[8]==0 && dd[16]==0 && dd[24]==0 && dd[32]==0 && dd[40]==0 && dd[48]==0 && dd[56]==0) {
            int dcterm = dd[0] * 4;
            v[0]=v[8]=v[16]=v[24]=v[32]=v[40]=v[48]=v[56] = dcterm;
        } else {
            JIDCT_1D(dd[0],dd[8],dd[16],dd[24],dd[32],dd[40],dd[48],dd[56])
            x0+=512; x1+=512; x2+=512; x3+=512;
            v[0]=(x0+t3)>>10; v[56]=(x0-t3)>>10; v[8]=(x1+t2)>>10; v[48]=(x1-t2)>>10;
            v[16]=(x2+t1)>>10; v[40]=(x2-t1)>>10; v[24]=(x3+t0)>>10; v[32]=(x3-t0)>>10;
        }
    }
    for (i = 0, v = val; i < 8; i++, v += 8, out += stride) {
        JIDCT_1D(v[0],v[1],v[2],v[3],v[4],v[5],v[6],v[7])
        x0 += 65536 + (128<<17); x1 += 65536 + (128<<17); x2 += 65536 + (128<<17); x3 += 65536 + (128<<17);
        out[0]=clamp8((x0+t3)>>17); out[7]=clamp8((x0-t3)>>17);
        out[1]=clamp8((x1+t2)>>17); out[6]=clamp8((x1-t2)>>17);
        out[2]=clamp8((x2+t1)>>17); out[5]=clamp8((x2-t1)>>17);
        out[3]=clamp8((x3+t0)>>17); out[4]=clamp8((x3-t0)>>17);
    }
}

static image_t *decode_jpeg(const uint8_t *d, uint32_t len) {
    if (len < 2 || d[0] != 0xFF || d[1] != 0xD8) return 0;
    jpg_t *j = (jpg_t *)kmalloc(sizeof(jpg_t));
    if (!j) return 0;
    memset(j, 0, sizeof(*j));
    j->d = d; j->len = len; j->pos = 2;

    int got_sof = 0;
    for (;;) {
        int m = jget8(j);
        if (m != 0xFF) { if (j->pos >= len) goto fail; continue; }
        while (m == 0xFF) m = jget8(j);
        if (m == 0xD9) goto fail;                         /* EOI before SOS */
        if (m == 0xDA) break;                             /* SOS -> decode  */
        int L = jget16(j); if (L < 2) goto fail;
        uint32_t seg_end = j->pos + (L - 2);
        if (m == 0xDB) {                                  /* DQT */
            while (j->pos < seg_end) {
                int q = jget8(j), id = q & 15, prec = q >> 4;
                if (id > 3) goto fail;
                for (int i = 0; i < 64; i++) j->dequant[id][i] = (uint16_t)(prec ? jget16(j) : jget8(j));
            }
        } else if (m == 0xC4) {                           /* DHT */
            while (j->pos < seg_end) {
                int tc = jget8(j), cls = tc >> 4, id = tc & 15;
                if (id > 3) goto fail;
                uint8_t cnt[16]; int total = 0;
                for (int i = 0; i < 16; i++) { cnt[i] = jget8(j); total += cnt[i]; }
                jhuff_t *hh = cls ? &j->huff_ac[id] : &j->huff_dc[id];
                if (!jbuild_huff(hh, cnt)) goto fail;
                for (int i = 0; i < total; i++) hh->values[i] = jget8(j);
            }
        } else if (m == 0xC0 || m == 0xC1) {              /* baseline / extended SOF */
            jget8(j);                                     /* precision */
            j->img_h = jget16(j); j->img_w = jget16(j);
            j->ncomp = jget8(j);
            if (j->ncomp != 1 && j->ncomp != 3) goto fail;
            if ((uint64_t)j->img_w * j->img_h > IMG_MAX_PIXELS) goto fail;
            for (int c = 0; c < j->ncomp; c++) {
                j->comp[c].id = jget8(j);
                int hv = jget8(j); j->comp[c].h = hv >> 4; j->comp[c].v = hv & 15;
                j->comp[c].qt = jget8(j);
            }
            got_sof = 1;
        } else if (m == 0xC2) { goto fail; }              /* progressive: unsupported */
        else if (m == 0xDD) { j->restart = jget16(j); }   /* DRI */
        /* APPn, COM, etc: skipped by seg_end */
        j->pos = seg_end;
    }
    if (!got_sof) goto fail;

    /* SOS header */
    {
        int L = jget16(j); (void)L;
        int ns = jget8(j);
        for (int i = 0; i < ns; i++) {
            int cid = jget8(j), td = jget8(j);
            for (int c = 0; c < j->ncomp; c++) if (j->comp[c].id == cid) { j->comp[c].dc = td >> 4; j->comp[c].ac = td & 15; }
        }
        jget8(j); jget8(j); jget8(j);                     /* Ss, Se, Ah/Al */
    }

    /* sampling geometry */
    j->hmax = j->vmax = 1;
    for (int c = 0; c < j->ncomp; c++) { if (j->comp[c].h > j->hmax) j->hmax = j->comp[c].h; if (j->comp[c].v > j->vmax) j->vmax = j->comp[c].v; }
    j->mcu_w = j->hmax * 8; j->mcu_h = j->vmax * 8;
    j->mcus_x = (j->img_w + j->mcu_w - 1) / j->mcu_w;
    j->mcus_y = (j->img_h + j->mcu_h - 1) / j->mcu_h;
    for (int c = 0; c < j->ncomp; c++) {
        int cw = j->mcus_x * j->comp[c].h * 8, ch = j->mcus_y * j->comp[c].v * 8;
        j->comp[c].w = cw;
        j->comp[c].data = (uint8_t *)kmalloc((uint32_t)cw * ch);
        if (!j->comp[c].data) goto fail;
    }

    /* entropy-decode every MCU */
    j->code_bits = 0; j->code_buffer = 0; j->marker = 0; j->nomore = 0;
    for (int c = 0; c < j->ncomp; c++) j->comp[c].dcpred = 0;
    int todo = j->restart ? j->restart : 0x7fffffff;
    short data[64];
    for (int my = 0; my < j->mcus_y; my++) {
        for (int mx = 0; mx < j->mcus_x; mx++) {
            for (int c = 0; c < j->ncomp; c++) {
                for (int vy = 0; vy < j->comp[c].v; vy++) {
                    for (int hx = 0; hx < j->comp[c].h; hx++) {
                        if (!jdecode_block(j, data, &j->huff_dc[j->comp[c].dc], &j->huff_ac[j->comp[c].ac], j->dequant[j->comp[c].qt]))
                            goto fail;
                        int diff = data[0];
                        j->comp[c].dcpred += diff;
                        data[0] = (short)(j->comp[c].dcpred * j->dequant[j->comp[c].qt][0]);
                        int bx = (mx * j->comp[c].h + hx) * 8;
                        int by = (my * j->comp[c].v + vy) * 8;
                        jidct(j->comp[c].data + by * j->comp[c].w + bx, j->comp[c].w, data);
                    }
                }
            }
            if (--todo <= 0) {
                /* restart marker: realign and reset predictors */
                if (j->code_bits < 24) jgrow(j);
                int mk = j->marker;
                if (mk >= 0xD0 && mk <= 0xD7) {
                    j->code_bits = 0; j->code_buffer = 0; j->marker = 0; j->nomore = 0;
                    for (int c = 0; c < j->ncomp; c++) j->comp[c].dcpred = 0;
                    todo = j->restart;
                }
            }
        }
    }

    /* assemble RGB(A) with nearest-neighbour upsampling */
    int W = j->img_w, H = j->img_h;
    uint32_t *px = (uint32_t *)kmalloc((uint32_t)W * H * 4);
    if (!px) goto fail;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (j->ncomp == 1) {
                uint8_t g = j->comp[0].data[y * j->comp[0].w + x];
                px[y*W+x] = argb(255, g, g, g);
            } else {
                int yw = j->comp[0].w;
                int yo = j->comp[0].data[y * yw + x];
                int cx = x * j->comp[1].h / j->hmax, cy = y * j->comp[1].v / j->vmax;
                int cb = j->comp[1].data[cy * j->comp[1].w + cx] - 128;
                int crx = x * j->comp[2].h / j->hmax, cry = y * j->comp[2].v / j->vmax;
                int cr = j->comp[2].data[cry * j->comp[2].w + crx] - 128;
                int r = yo + ((91881 * cr) >> 16);
                int g = yo - ((22554 * cb + 46802 * cr) >> 16);
                int b = yo + ((116130 * cb) >> 16);
                px[y*W+x] = argb(255, clamp8(r), clamp8(g), clamp8(b));
            }
        }
    }
    for (int c = 0; c < j->ncomp; c++) if (j->comp[c].data) kfree(j->comp[c].data);
    kfree(j);
    return finalize(px, W, H);

fail:
    for (int c = 0; c < 4; c++) if (j->comp[c].data) kfree(j->comp[c].data);
    kfree(j);
    return 0;
}

/* ===========================================================================
 *  Dispatch
 * ===========================================================================*/
image_t *image_decode(const uint8_t *buf, uint32_t len) {
    if (!buf || len < 4) return 0;
    if (buf[0]==0x89 && buf[1]=='P' && buf[2]=='N' && buf[3]=='G') return decode_png(buf, len);
    if (buf[0]==0xFF && buf[1]==0xD8)                                return decode_jpeg(buf, len);
    if (buf[0]=='G' && buf[1]=='I' && buf[2]=='F')                  return decode_gif(buf, len);
    if (buf[0]=='B' && buf[1]=='M')                                  return decode_bmp(buf, len);
    return decode_tga(buf, len);   /* no magic: validated fallback */
}

void image_free(image_t *im) {
    if (!im) return;
    if (im->px) kfree(im->px);
    kfree(im);
}
