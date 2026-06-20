#include <stdint.h>
#include "net.h"
#include "firewall.h"
#include "string.h"

/* ===========================================================================
 *  Real packet filter. ip_input()/ip_output() consult fw_check() for every
 *  datagram; a BLOCK verdict drops it on the floor. Rules match on direction,
 *  protocol, remote address (with CIDR mask) and remote port. First match
 *  wins; no match falls through to the default policy.
 * ===========================================================================*/
#define FW_MAX 32
#define FW_TEXT 56

struct fw_rule {
    uint8_t  dir;        /* FW_IN / FW_OUT / FW_ANY            */
    uint8_t  action;     /* 1 = allow, 0 = block              */
    uint8_t  proto;      /* 0 = any, else IPPROTO_*            */
    uint8_t  has_addr;
    uint8_t  has_port;
    uint32_t addr;       /* host order                        */
    uint32_t mask;       /* host order, CIDR                  */
    uint16_t port;       /* host order                        */
    char     text[FW_TEXT];
};

static struct fw_rule rules[FW_MAX];
static int      rule_count;
static int      enabled;
static int      default_allow = 1;        /* fail-open until a policy is set */
static uint32_t stat_passed, stat_blocked;

int  fw_enabled(void)        { return enabled; }
void fw_set_enabled(int on)  { enabled = on ? 1 : 0; }
void fw_set_default(int a)   { default_allow = a ? 1 : 0; }
int  fw_get_default(void)    { return default_allow; }
int  fw_count(void)          { return rule_count; }
void fw_clear(void)          { rule_count = 0; }
const char *fw_rule_text(int i) { return (i >= 0 && i < rule_count) ? rules[i].text : ""; }
void fw_stats(uint32_t *p, uint32_t *b) { if (p) *p = stat_passed; if (b) *b = stat_blocked; }

static int rule_matches(const struct fw_rule *r, int dir, uint8_t proto,
                        uint32_t peer, uint16_t port) {
    if (r->dir != FW_ANY && r->dir != (uint8_t)dir) return 0;
    if (r->proto && r->proto != proto) return 0;
    if (r->has_addr && ((peer & r->mask) != (r->addr & r->mask))) return 0;
    if (r->has_port && r->port != port) return 0;
    return 1;
}

int fw_check(int dir, uint8_t proto, uint32_t peer, uint16_t port) {
    if (!enabled) return 1;
    for (int i = 0; i < rule_count; i++) {
        if (rule_matches(&rules[i], dir, proto, peer, port)) {
            if (rules[i].action) stat_passed++; else stat_blocked++;
            return rules[i].action;
        }
    }
    if (default_allow) stat_passed++; else stat_blocked++;
    return default_allow;
}

static int parse_port(const char *s, uint16_t *out) {
    int p = 0; const char *q = s;
    if (!*q) return 0;
    while (*q >= '0' && *q <= '9') { p = p * 10 + (*q++ - '0'); if (p > 65535) return 0; }
    if (*q || p <= 0) return 0;
    *out = (uint16_t)p;
    return 1;
}

/* "a.b.c.d" or "a.b.c.d/prefix" -> addr + CIDR mask (host order) */
static int parse_addr(const char *s, uint32_t *addr, uint32_t *mask) {
    char buf[24]; int i = 0;
    for (; s[i] && s[i] != '/' && i < 23; i++) buf[i] = s[i];
    buf[i] = 0;
    int prefix = 32;
    if (s[i] == '/') {
        prefix = 0;
        for (const char *p = s + i + 1; *p; p++) {
            if (*p < '0' || *p > '9') return 0;
            prefix = prefix * 10 + (*p - '0');
        }
    }
    if (prefix < 0 || prefix > 32) return 0;
    int ok; uint32_t a = net_parse_ipv4(buf, &ok);
    if (!ok) return 0;
    *addr = a;
    *mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
    return 1;
}

int fw_add(int ntok, char **tok, const char *raw) {
    if (rule_count >= FW_MAX) return -2;
    struct fw_rule r;
    memset(&r, 0, sizeof r);
    r.dir = FW_ANY;
    r.mask = 0xFFFFFFFFu;
    int action_set = 0;

    for (int i = 0; i < ntok; i++) {
        char *t = tok[i];
        if      (strcmp(t, "allow") == 0)              { r.action = 1; action_set = 1; }
        else if (strcmp(t, "block") == 0 ||
                 strcmp(t, "deny")  == 0)              { r.action = 0; action_set = 1; }
        else if (strcmp(t, "in")   == 0)               r.dir = FW_IN;
        else if (strcmp(t, "out")  == 0)               r.dir = FW_OUT;
        else if (strcmp(t, "tcp")  == 0)               r.proto = IPPROTO_TCP;
        else if (strcmp(t, "udp")  == 0)               r.proto = IPPROTO_UDP;
        else if (strcmp(t, "icmp") == 0)               r.proto = IPPROTO_ICMP;
        else if (strcmp(t, "ip")   == 0 && i + 1 < ntok) {
            if (!parse_addr(tok[++i], &r.addr, &r.mask)) return -1;
            r.has_addr = 1;
        }
        else if (strcmp(t, "port") == 0 && i + 1 < ntok) {
            if (!parse_port(tok[++i], &r.port)) return -1;
            r.has_port = 1;
        }
        else return -1;        /* unknown token */
    }
    if (!action_set) return -1;

    uint32_t k = 0;
    if (raw) for (; raw[k] && k < FW_TEXT - 1; k++) r.text[k] = raw[k];
    r.text[k] = 0;

    rules[rule_count++] = r;
    return 0;
}
