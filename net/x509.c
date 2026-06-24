#include <stdint.h>
#include "x509.h"
#include "crypto.h"
#include "rsa.h"
#include "hw.h"            /* rtc_now */
#include "string.h"
#include "kprintf.h"

int p384_ecdsa_verify(const uint8_t *hash, uint32_t hlen,
                      const uint8_t pub[97], const uint8_t *r, const uint8_t *s);

/* ===========================================================================
 *  X.509 v3 certificate parsing + chain validation (DER).
 *
 *  Scope: what a TLS client needs. Per cert we extract the TBS bytes, the
 *  signature + its algorithm, issuer/subject DNs (raw, for linkage), validity
 *  window, the SubjectPublicKeyInfo, and the SubjectAltName dNSNames. We then
 *  enforce hostname, dates, and the chain signatures (RSA PKCS#1v1.5 / PSS and
 *  ECDSA P-256). Trust anchors live in a small runtime store (see x509_add_root).
 * ===========================================================================*/

#define X509_DEBUG 0
#if X509_DEBUG
#  define XDBG(...) kprintf(__VA_ARGS__)
#else
#  define XDBG(...) ((void)0)
#endif

/* signature algorithms we recognise */
enum { SIG_UNKNOWN = 0, SIG_RSA_SHA256, SIG_RSA_SHA384, SIG_RSA_PSS,
       SIG_ECDSA_SHA256, SIG_ECDSA_SHA384 };
/* public-key types */
enum { PK_NONE = 0, PK_RSA, PK_EC };

typedef struct {
    const uint8_t *tbs;     uint32_t tbs_len;     /* signed bytes (incl SEQ hdr) */
    const uint8_t *sig;     uint32_t sig_len;     /* signatureValue contents     */
    int            sig_alg;
    const uint8_t *issuer;  uint32_t issuer_len;  /* raw Name TLV                */
    const uint8_t *subject; uint32_t subject_len;
    const uint8_t *spki;    uint32_t spki_len;    /* raw SubjectPublicKeyInfo    */
    int            pk_type;
    const uint8_t *rsa_n;   uint32_t rsa_n_len;
    const uint8_t *rsa_e;   uint32_t rsa_e_len;
    const uint8_t *ec_point;uint32_t ec_point_len;/* 0x04||X||Y                  */
    uint64_t       not_before, not_after;
    const uint8_t *san;     uint32_t san_len;      /* GeneralNames SEQ contents  */
    int            is_ca;
} cert_t;

/* --------------------------------- DER ---------------------------------- */
/* Read one TLV at *p (< end). Returns tag, content pointer/len, advances *p
 * past the element. -1 on malformed input. */
static int der_tlv(const uint8_t **p, const uint8_t *end,
                   uint8_t *tag, const uint8_t **body, uint32_t *blen) {
    const uint8_t *q = *p;
    if (q + 2 > end) return -1;
    *tag = *q++;
    uint32_t len = *q++;
    if (len & 0x80) {
        uint32_t nb = len & 0x7F;
        if (nb == 0 || nb > 4 || q + nb > end) return -1;
        len = 0;
        for (uint32_t i = 0; i < nb; i++) len = (len << 8) | *q++;
    }
    if (q + len > end) return -1;
    *body = q; *blen = len; *p = q + len;
    return 0;
}
/* Descend into a constructed element: returns content range [body,body+blen). */
static int der_into(const uint8_t **p, const uint8_t *end, uint8_t want,
                    const uint8_t **body, const uint8_t **bend) {
    uint8_t t; uint32_t l;
    if (der_tlv(p, end, &t, body, &l) != 0) return -1;
    if (want && t != want) return -1;
    *bend = *body + l;
    return 0;
}

/* OID byte strings (content only, no tag/len) */
static const uint8_t OID_RSA[]        = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01};
static const uint8_t OID_RSA_SHA256[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B};
static const uint8_t OID_RSA_SHA384[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C};
static const uint8_t OID_RSA_PSS[]    = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0A};
static const uint8_t OID_EC_PUB[]     = {0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01};
static const uint8_t OID_ECDSA_S256[] = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02};
static const uint8_t OID_ECDSA_S384[] = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03};
static const uint8_t OID_SAN[]        = {0x55,0x1D,0x11};
static const uint8_t OID_BASIC[]      = {0x55,0x1D,0x13};

static int oid_eq(const uint8_t *o, uint32_t ol, const uint8_t *ref, uint32_t rl) {
    return ol == rl && memcmp(o, ref, rl) == 0;
}

/* AlgorithmIdentifier ::= SEQ { OID, params }. Map to our SIG_* enum. */
static int parse_sig_alg(const uint8_t *p, const uint8_t *end) {
    const uint8_t *b, *be; if (der_into(&p, end, 0x30, &b, &be) != 0) return SIG_UNKNOWN;
    uint8_t t; const uint8_t *oid; uint32_t ol;
    if (der_tlv(&b, be, &t, &oid, &ol) != 0 || t != 0x06) return SIG_UNKNOWN;
    if (oid_eq(oid, ol, OID_RSA_SHA256, sizeof OID_RSA_SHA256)) return SIG_RSA_SHA256;
    if (oid_eq(oid, ol, OID_RSA_SHA384, sizeof OID_RSA_SHA384)) return SIG_RSA_SHA384;
    if (oid_eq(oid, ol, OID_RSA_PSS,    sizeof OID_RSA_PSS))    return SIG_RSA_PSS;
    if (oid_eq(oid, ol, OID_ECDSA_S256, sizeof OID_ECDSA_S256)) return SIG_ECDSA_SHA256;
    if (oid_eq(oid, ol, OID_ECDSA_S384, sizeof OID_ECDSA_S384)) return SIG_ECDSA_SHA384;
    return SIG_UNKNOWN;
}

/* Parse a Time (UTCTime/GeneralizedTime) into a sortable yyyymmddhhmmss code. */
#define DIG(k) ((uint64_t)(b[k] - '0'))
static uint64_t parse_time(uint8_t tag, const uint8_t *b, uint32_t l) {
    /* read 2- or 4-digit year then MMDDHHMMSS */
    uint32_t i = 0; uint64_t year;
    if (tag == 0x17) {                          /* UTCTime: YY */
        if (l < 12) return 0;
        uint64_t yy = DIG(0) * 10 + DIG(1); year = yy < 50 ? 2000 + yy : 1900 + yy; i = 2;
    } else {                                    /* GeneralizedTime: YYYY */
        if (l < 14) return 0;
        year = DIG(0)*1000 + DIG(1)*100 + DIG(2)*10 + DIG(3); i = 4;
    }
    uint64_t mon = DIG(i)*10+DIG(i+1); i += 2;
    uint64_t day = DIG(i)*10+DIG(i+1); i += 2;
    uint64_t hh  = DIG(i)*10+DIG(i+1); i += 2;
    uint64_t mm  = DIG(i)*10+DIG(i+1); i += 2;
    uint64_t ss  = DIG(i)*10+DIG(i+1);
    return ((((year*100+mon)*100+day)*100+hh)*100+mm)*100+ss;
}
#undef DIG
static uint64_t rtc_code(void) {
    struct rtc_time t; rtc_now(&t);
    return ((((( uint64_t)t.year*100+t.mon)*100+t.day)*100+t.hour)*100+t.min)*100+t.sec;
}

/* Parse SubjectPublicKeyInfo (raw TLV) into key material. */
static int parse_spki(const uint8_t *spki, uint32_t spki_len, cert_t *c) {
    const uint8_t *p = spki, *end = spki + spki_len, *b, *be;
    if (der_into(&p, end, 0x30, &b, &be) != 0) return -1;     /* SPKI SEQ */
    const uint8_t *ab, *abe;
    if (der_into(&b, be, 0x30, &ab, &abe) != 0) return -1;    /* algorithm SEQ */
    uint8_t t; const uint8_t *oid; uint32_t ol;
    if (der_tlv(&ab, abe, &t, &oid, &ol) != 0 || t != 0x06) return -1;
    /* the BIT STRING with the key follows the algorithm SEQ */
    const uint8_t *key; uint32_t klen;
    if (der_tlv(&b, be, &t, &key, &klen) != 0 || t != 0x03 || klen < 1) return -1;
    key++; klen--;                                            /* drop unused-bits byte */
    if (oid_eq(oid, ol, OID_RSA, sizeof OID_RSA)) {
        c->pk_type = PK_RSA;
        const uint8_t *kp = key, *kend = key + klen, *sb, *sbe;
        if (der_into(&kp, kend, 0x30, &sb, &sbe) != 0) return -1;
        const uint8_t *n; uint32_t nl;
        if (der_tlv(&sb, sbe, &t, &n, &nl) != 0 || t != 0x02) return -1;
        while (nl > 0 && n[0] == 0x00) { n++; nl--; }         /* strip sign byte */
        const uint8_t *e; uint32_t el;
        if (der_tlv(&sb, sbe, &t, &e, &el) != 0 || t != 0x02) return -1;
        c->rsa_n = n; c->rsa_n_len = nl; c->rsa_e = e; c->rsa_e_len = el;
        return 0;
    }
    if (oid_eq(oid, ol, OID_EC_PUB, sizeof OID_EC_PUB)) {
        c->pk_type = PK_EC;
        if ((klen == 65 || klen == 97) && key[0] == 0x04) {   /* P-256 or P-384 */
            c->ec_point = key; c->ec_point_len = klen; return 0;
        }
        return -1;
    }
    return -1;
}

/* Walk the [3] extensions for SAN and basicConstraints. */
static void parse_extensions(const uint8_t *p, const uint8_t *end, cert_t *c) {
    const uint8_t *b, *be;
    if (der_into(&p, end, 0xA3, &b, &be) != 0) return;        /* [3] EXPLICIT */
    const uint8_t *sb, *sbe;
    if (der_into(&b, be, 0x30, &sb, &sbe) != 0) return;       /* SEQ OF Extension */
    while (sb < sbe) {
        const uint8_t *eb, *ebe;
        if (der_into(&sb, sbe, 0x30, &eb, &ebe) != 0) return;
        uint8_t t; const uint8_t *oid; uint32_t ol;
        if (der_tlv(&eb, ebe, &t, &oid, &ol) != 0 || t != 0x06) continue;
        /* optional critical BOOLEAN, then OCTET STRING value */
        const uint8_t *val; uint32_t vl;
        if (der_tlv(&eb, ebe, &t, &val, &vl) != 0) continue;
        if (t == 0x01) { if (der_tlv(&eb, ebe, &t, &val, &vl) != 0) continue; }
        if (t != 0x04) continue;
        if (oid_eq(oid, ol, OID_SAN, sizeof OID_SAN)) {
            const uint8_t *vp = val, *vend = val + vl, *gb, *gbe;
            if (der_into(&vp, vend, 0x30, &gb, &gbe) == 0) { c->san = gb; c->san_len = (uint32_t)(gbe - gb); }
        } else if (oid_eq(oid, ol, OID_BASIC, sizeof OID_BASIC)) {
            const uint8_t *vp = val, *vend = val + vl, *cb, *cbe;
            if (der_into(&vp, vend, 0x30, &cb, &cbe) == 0) {
                uint8_t bt; const uint8_t *bb; uint32_t bl;
                if (der_tlv(&cb, cbe, &bt, &bb, &bl) == 0 && bt == 0x01 && bl == 1 && bb[0]) c->is_ca = 1;
            }
        }
    }
}

/* Parse a single certificate (DER) into c. Returns 0 on success. */
static int parse_cert(const uint8_t *der, uint32_t len, cert_t *c) {
    memset(c, 0, sizeof *c);
    const uint8_t *p = der, *end = der + len, *cb, *cbe;
    if (der_into(&p, end, 0x30, &cb, &cbe) != 0) return -1;   /* Certificate SEQ */

    /* tbsCertificate: capture the whole element (header included) for hashing */
    const uint8_t *tbs_start = cb;
    const uint8_t *tb, *tbe;
    if (der_into(&cb, cbe, 0x30, &tb, &tbe) != 0) return -1;
    c->tbs = tbs_start; c->tbs_len = (uint32_t)(cb - tbs_start);

    /* signatureAlgorithm + signatureValue (outer) */
    c->sig_alg = parse_sig_alg(cb, cbe);
    { uint8_t t; const uint8_t *b; uint32_t l; const uint8_t *q = cb;
      if (der_tlv(&q, cbe, &t, &b, &l) != 0) return -1;        /* skip alg SEQ */
      if (der_tlv(&q, cbe, &t, &b, &l) != 0 || t != 0x03 || l < 1) return -1;
      c->sig = b + 1; c->sig_len = l - 1; }                    /* drop unused-bits byte */

    /* inside TBS */
    const uint8_t *q = tb;
    uint8_t t; const uint8_t *b; uint32_t l;
    /* optional [0] version */
    { const uint8_t *save = q; if (der_tlv(&q, tbe, &t, &b, &l) != 0) return -1;
      if (t != 0xA0) q = save; }                               /* not present: rewind */
    if (der_tlv(&q, tbe, &t, &b, &l) != 0 || t != 0x02) return -1;   /* serial */
    { const uint8_t *ss = q; if (der_tlv(&q, tbe, &t, &b, &l) != 0 || t != 0x30) return -1; (void)ss; } /* signature */
    /* issuer (raw TLV) */
    { const uint8_t *is = q; if (der_tlv(&q, tbe, &t, &b, &l) != 0 || t != 0x30) return -1;
      c->issuer = is; c->issuer_len = (uint32_t)(q - is); }
    /* validity */
    { const uint8_t *vb, *vbe; const uint8_t *vq = q;
      if (der_into(&vq, tbe, 0x30, &vb, &vbe) != 0) return -1;
      uint8_t tt; const uint8_t *tbz; uint32_t tl;
      if (der_tlv(&vb, vbe, &tt, &tbz, &tl) != 0) return -1; c->not_before = parse_time(tt, tbz, tl);
      if (der_tlv(&vb, vbe, &tt, &tbz, &tl) != 0) return -1; c->not_after  = parse_time(tt, tbz, tl);
      q = vq; }
    /* subject (raw TLV) */
    { const uint8_t *su = q; if (der_tlv(&q, tbe, &t, &b, &l) != 0 || t != 0x30) return -1;
      c->subject = su; c->subject_len = (uint32_t)(q - su); }
    /* subjectPublicKeyInfo (raw TLV). An unsupported key type (e.g. P-384) is
     * not fatal here -- it only matters if we must verify with this key. */
    { const uint8_t *sp = q; if (der_tlv(&q, tbe, &t, &b, &l) != 0 || t != 0x30) return -1;
      c->spki = sp; c->spki_len = (uint32_t)(q - sp);
      if (parse_spki(c->spki, c->spki_len, c) != 0) c->pk_type = PK_NONE; }
    /* remaining: optional [1] [2] then [3] extensions */
    parse_extensions(q, tbe, c);
    return 0;
}

/* ----------------------- signature verification ------------------------ */
/* Verify `sig` over `msg` using issuer public key in `ic`, per sig_alg. */
static int verify_with(const cert_t *ic, int sig_alg,
                       const uint8_t *msg, uint32_t mlen,
                       const uint8_t *sig, uint32_t sig_len) {
    int h384 = (sig_alg == SIG_RSA_SHA384 || sig_alg == SIG_ECDSA_SHA384);
    uint8_t hash[48]; uint32_t hlen = h384 ? 48 : 32;
    if (h384) { sha512_ctx s; sha384_init(&s); sha512_update(&s, msg, mlen); sha384_final(&s, hash); }
    else      { sha256_ctx s; sha256_init(&s); sha256_update(&s, msg, mlen); sha256_final(&s, hash); }

    if (sig_alg == SIG_RSA_SHA256 || sig_alg == SIG_RSA_SHA384) {
        if (ic->pk_type != PK_RSA) return -1;
        return rsa_verify_pkcs1(ic->rsa_n, ic->rsa_n_len, ic->rsa_e, ic->rsa_e_len,
                                sig, sig_len, hash, hlen, h384);
    }
    if (sig_alg == SIG_RSA_PSS) {
        if (ic->pk_type != PK_RSA) return -1;          /* assume SHA-256 PSS */
        return rsa_verify_pss(ic->rsa_n, ic->rsa_n_len, ic->rsa_e, ic->rsa_e_len,
                              sig, sig_len, hash, 32, 0);
    }
    if (sig_alg == SIG_ECDSA_SHA256 || sig_alg == SIG_ECDSA_SHA384) {
        if (ic->pk_type != PK_EC) return -1;
        uint32_t clen = (ic->ec_point_len - 1) / 2;      /* 32 (P-256) or 48 (P-384) */
        /* ECDSA-Sig-Value ::= SEQ { r INTEGER, s INTEGER } */
        const uint8_t *p = sig, *e = sig + sig_len, *sb, *sbe;
        if (der_into(&p, e, 0x30, &sb, &sbe) != 0) return -1;
        uint8_t t; const uint8_t *rb, *ssb; uint32_t rl, sl;
        if (der_tlv(&sb, sbe, &t, &rb, &rl) != 0 || t != 0x02) return -1;
        if (der_tlv(&sb, sbe, &t, &ssb, &sl) != 0 || t != 0x02) return -1;
        uint8_t r[48], s[48]; memset(r, 0, 48); memset(s, 0, 48);
        while (rl > 0 && rb[0] == 0) { rb++; rl--; }
        while (sl > 0 && ssb[0] == 0) { ssb++; sl--; }
        if (rl > clen || sl > clen) return -1;
        memcpy(r + (clen - rl), rb, rl); memcpy(s + (clen - sl), ssb, sl);
        return clen == 48 ? p384_ecdsa_verify(hash, hlen, ic->ec_point, r, s)
                          : p256_ecdsa_verify(hash, hlen, ic->ec_point, r, s);
    }
    return -1;
}

/* ------------------------------ hostname ------------------------------- */
static int ci_eq(const uint8_t *a, uint32_t al, const char *b) {
    uint32_t i = 0;
    for (; i < al && b[i]; i++) {
        char x = (char)a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return i == al && b[i] == 0;
}
/* Match host against a SAN dNSName entry (supports a leading "*." wildcard). */
static int dns_match(const uint8_t *pat, uint32_t pl, const char *host) {
    if (pl >= 2 && pat[0] == '*' && pat[1] == '.') {
        const char *dot = host; while (*dot && *dot != '.') dot++;
        if (*dot != '.') return 0;
        return ci_eq(pat + 1, pl - 1, dot);            /* "*.x" vs ".x" suffix */
    }
    return ci_eq(pat, pl, host);
}
static int hostname_ok(const cert_t *leaf, const char *host) {
    if (!leaf->san) return 0;
    const uint8_t *p = leaf->san, *e = leaf->san + leaf->san_len;
    while (p < e) {
        uint8_t t; const uint8_t *b; uint32_t l;
        if (der_tlv(&p, e, &t, &b, &l) != 0) break;
        if (t == 0x82 && dns_match(b, l, host)) return 1;   /* [2] dNSName */
    }
    return 0;
}

/* ------------------------------ trust store ---------------------------- */
#define MAX_ROOTS 8
#define ROOT_CAP  2048
static struct { uint8_t der[ROOT_CAP]; uint32_t len; cert_t c; int ok; } g_roots[MAX_ROOTS];
static int g_nroots = 0;

int x509_add_root(const uint8_t *der, uint32_t len) {
    if (g_nroots >= MAX_ROOTS || len > ROOT_CAP) return -1;
    memcpy(g_roots[g_nroots].der, der, len);
    g_roots[g_nroots].len = len;
    g_roots[g_nroots].ok = (parse_cert(g_roots[g_nroots].der, len, &g_roots[g_nroots].c) == 0);
    g_nroots++;
    return 0;
}
int x509_root_count(void) { return g_nroots; }

static int dn_eq(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl) {
    return al == bl && memcmp(a, b, al) == 0;
}
/* Find a trusted root whose subject == issuer DN and that signs `c`. */
static int anchored(const cert_t *c) {
    for (int i = 0; i < g_nroots; i++) {
        if (!g_roots[i].ok) continue;
        if (!dn_eq(c->issuer, c->issuer_len, g_roots[i].c.subject, g_roots[i].c.subject_len)) continue;
        if (verify_with(&g_roots[i].c, c->sig_alg, c->tbs, c->tbs_len, c->sig, c->sig_len) == 0) return 1;
    }
    return 0;
}

/* ------------------------------- entry --------------------------------- */
int x509_verify_certverify(const uint8_t *spki, uint32_t spki_len, uint16_t sigalg,
                           const uint8_t *content, uint32_t clen,
                           const uint8_t *sig, uint32_t slen) {
    cert_t leaf; memset(&leaf, 0, sizeof leaf);
    if (parse_spki(spki, spki_len, &leaf) != 0) return -1;
    switch (sigalg) {
        case 0x0403:  /* ecdsa_secp256r1_sha256 */
            if (leaf.pk_type != PK_EC) return -1;
            { uint8_t h[32]; sha256(content, clen, h);
              const uint8_t *p = sig, *e = sig + slen, *sb, *sbe;
              if (der_into(&p, e, 0x30, &sb, &sbe) != 0) return -1;
              uint8_t t; const uint8_t *rb, *ssb; uint32_t rl, sl;
              if (der_tlv(&sb, sbe, &t, &rb, &rl) != 0 || t != 0x02) return -1;
              if (der_tlv(&sb, sbe, &t, &ssb, &sl) != 0 || t != 0x02) return -1;
              uint8_t r[32], s[32]; memset(r,0,32); memset(s,0,32);
              while (rl > 0 && rb[0] == 0) { rb++; rl--; }
              while (sl > 0 && ssb[0] == 0) { ssb++; sl--; }
              if (rl > 32 || sl > 32) return -1;
              memcpy(r+(32-rl), rb, rl); memcpy(s+(32-sl), ssb, sl);
              return p256_ecdsa_verify(h, 32, leaf.ec_point, r, s); }
        case 0x0804:  /* rsa_pss_rsae_sha256 */
        case 0x0805:  /* rsa_pss_rsae_sha384 */
            if (leaf.pk_type != PK_RSA) return -1;
            { int h384 = (sigalg == 0x0805); uint8_t h[48]; uint32_t hl = h384 ? 48 : 32;
              if (h384) { sha512_ctx s; sha384_init(&s); sha512_update(&s, content, clen); sha384_final(&s, h); }
              else      sha256(content, clen, h);
              return rsa_verify_pss(leaf.rsa_n, leaf.rsa_n_len, leaf.rsa_e, leaf.rsa_e_len,
                                    sig, slen, h, hl, h384); }
        case 0x0401:  /* rsa_pkcs1_sha256 */
        case 0x0501:  /* rsa_pkcs1_sha384 */
            if (leaf.pk_type != PK_RSA) return -1;
            { int h384 = (sigalg == 0x0501); uint8_t h[48]; uint32_t hl = h384 ? 48 : 32;
              if (h384) { sha512_ctx s; sha384_init(&s); sha512_update(&s, content, clen); sha384_final(&s, h); }
              else      sha256(content, clen, h);
              return rsa_verify_pkcs1(leaf.rsa_n, leaf.rsa_n_len, leaf.rsa_e, leaf.rsa_e_len,
                                      sig, slen, h, hl, h384); }
        default: return -1;
    }
}

int tls_verify_chain(const uint8_t *certmsg, uint32_t len, const char *host,
                     uint8_t *spki_out, uint32_t *spki_len, uint32_t spki_cap) {
    /* TLS 1.3 Certificate message: cert_request_context (1-byte len + data),
     * then 3-byte cert_list length, then entries of (3-byte len + cert + 2-byte
     * ext len + exts). */
    const uint8_t *p = certmsg, *end = certmsg + len;
    if (p >= end) return -1;
    uint8_t ctxl = *p++; p += ctxl;
    if (p + 3 > end) return -1;
    uint32_t listlen = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; p += 3;
    const uint8_t *lend = p + listlen; if (lend > end) lend = end;

    cert_t chain[6]; int n = 0;
    while (p + 3 <= lend && n < 6) {
        uint32_t cl = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; p += 3;
        if (p + cl > lend) break;
        if (parse_cert(p, cl, &chain[n]) != 0) { XDBG("x509: parse cert %d failed\n", n); return -1; }
        p += cl;
        if (p + 2 > lend) break;
        uint32_t extl = ((uint32_t)p[0] << 8) | p[1]; p += 2 + extl;  /* skip CertificateEntry extensions */
        n++;
    }
    if (n == 0) { XDBG("x509: empty chain\n"); return -1; }

    /* hostname */
    if (!hostname_ok(&chain[0], host)) { XDBG("x509: hostname '%s' not in SAN\n", host); return -1; }

    /* validity dates for every cert */
    uint64_t now = rtc_code();
    for (int i = 0; i < n; i++) {
        if (now < chain[i].not_before || now > chain[i].not_after) {
            XDBG("x509: cert %d outside validity window\n", i); return -1;
        }
    }

    /* chain linkage: cert[i] signed by cert[i+1] */
    for (int i = 0; i + 1 < n; i++) {
        if (!dn_eq(chain[i].issuer, chain[i].issuer_len, chain[i+1].subject, chain[i+1].subject_len)) {
            XDBG("x509: issuer/subject break at %d\n", i); return -1;
        }
        if (verify_with(&chain[i+1], chain[i].sig_alg, chain[i].tbs, chain[i].tbs_len,
                        chain[i].sig, chain[i].sig_len) != 0) {
            XDBG("x509: signature %d not verified by issuer\n", i); return -1;
        }
    }

    /* trust anchor */
    if (g_nroots > 0) {
        if (!anchored(&chain[n-1])) { XDBG("x509: chain not anchored to a trusted root\n"); return -1; }
        XDBG("x509: host+dates+chain+anchor OK (%d certs)\n", n);
    } else {
        XDBG("x509: host+dates+chain OK (%d certs); anchor unverified (empty trust store)\n", n);
    }

    /* hand the leaf SPKI back for CertificateVerify */
    if (chain[0].spki_len > spki_cap) return -1;
    memcpy(spki_out, chain[0].spki, chain[0].spki_len);
    *spki_len = chain[0].spki_len;
    return 0;
}
