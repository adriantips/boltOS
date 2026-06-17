#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "pit.h"
#include "string.h"

/* ===========================================================================
 *  Tiny DNS resolver: one A-record query over UDP/53 to net_dns, blocking until
 *  the reply arrives or we time out. Handles name compression in the answer
 *  section by skipping compressed/literal names; returns the first A record.
 * ===========================================================================*/

static volatile uint32_t dns_result;
static volatile int      dns_done;
static volatile uint16_t dns_want_id;

/* encode "www.example.com" as length-prefixed labels into out; return length */
static int encode_qname(const char *host, uint8_t *out) {
    int o = 0;
    const char *s = host;
    while (*s) {
        const char *dot = s;
        while (*dot && *dot != '.') dot++;
        int seglen = (int)(dot - s);
        if (seglen <= 0 || seglen > 63) return -1;
        out[o++] = (uint8_t)seglen;
        for (int i = 0; i < seglen; i++) out[o++] = (uint8_t)s[i];
        s = (*dot == '.') ? dot + 1 : dot;
    }
    out[o++] = 0;
    return o;
}

/* advance past a (possibly compressed) name at off; return offset after it */
static uint32_t skip_name(const uint8_t *msg, uint32_t len, uint32_t off) {
    while (off < len) {
        uint8_t b = msg[off];
        if ((b & 0xC0) == 0xC0) return off + 2;       /* compression pointer */
        if (b == 0) return off + 1;                   /* end of name         */
        off += 1u + b;                                /* literal label       */
    }
    return len;
}

static void dns_recv(uint32_t src_ip, uint16_t sport, const uint8_t *msg, uint16_t len) {
    (void)src_ip; (void)sport;
    if (len < 12) return;
    uint16_t id = (uint16_t)((msg[0] << 8) | msg[1]);
    if (id != dns_want_id) return;
    uint16_t qd = (uint16_t)((msg[4] << 8) | msg[5]);
    uint16_t an = (uint16_t)((msg[6] << 8) | msg[7]);
    if ((msg[3] & 0x0F) != 0) { dns_done = 1; return; } /* RCODE != 0 */

    uint32_t off = 12;
    for (uint16_t i = 0; i < qd; i++) off = skip_name(msg, len, off) + 4; /* +QTYPE+QCLASS */

    for (uint16_t i = 0; i < an && off + 10 <= len; i++) {
        off = skip_name(msg, len, off);
        if (off + 10 > len) break;
        uint16_t type   = (uint16_t)((msg[off] << 8) | msg[off + 1]);
        uint16_t rdlen  = (uint16_t)((msg[off + 8] << 8) | msg[off + 9]);
        uint32_t rdata  = off + 10;
        if (type == 1 && rdlen == 4 && rdata + 4 <= len) {     /* A record */
            dns_result = ((uint32_t)msg[rdata] << 24) | ((uint32_t)msg[rdata + 1] << 16) |
                         ((uint32_t)msg[rdata + 2] << 8) | (uint32_t)msg[rdata + 3];
            dns_done = 1;
            return;
        }
        off = rdata + rdlen;
    }
    dns_done = 1;                                       /* answered, but no A */
}

int dns_resolve(const char *host, uint32_t *ip_out, uint32_t timeout_ms) {
    struct netif *nif = netif_default();
    if (!nif) return 0;

    /* a literal dotted quad needs no lookup */
    int ok = 0;
    uint32_t lit = net_parse_ipv4(host, &ok);
    if (ok) { *ip_out = lit; return 1; }

    uint8_t q[300];
    static uint16_t seq = 0x1000;
    uint16_t id = ++seq;
    q[0] = id >> 8; q[1] = id & 0xFF;
    q[2] = 0x01; q[3] = 0x00;       /* recursion desired */
    q[4] = 0; q[5] = 1;             /* QDCOUNT = 1 */
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    int qn = encode_qname(host, q + 12);
    if (qn < 0) return 0;
    int o = 12 + qn;
    q[o++] = 0; q[o++] = 1;         /* QTYPE  = A  */
    q[o++] = 0; q[o++] = 1;         /* QCLASS = IN */

    uint16_t sport = 50000 + (id & 0x0FFF);
    dns_done = 0; dns_result = 0; dns_want_id = id;
    udp_listen(sport, dns_recv);

    int got = 0;
    for (int attempt = 0; attempt < 3 && !got; attempt++) {
        udp_send(nif, net_dns, sport, 53, q, (uint16_t)o);
        uint64_t start = pit_ticks();
        while (pit_ticks() - start < timeout_ms / 3 + 1) {
            netif_poll_all();
            if (dns_done) { got = 1; break; }
            __asm__ volatile("hlt");
        }
    }

    udp_unlisten(sport);
    if (got && dns_result) { *ip_out = dns_result; return 1; }
    return 0;
}
