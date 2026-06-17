#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "pit.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  Minimal active-open TCP client. Just enough to fetch an HTTP page: the
 *  three-way handshake, in-order receive with cumulative ACKs, a single-flight
 *  data send (the request), and an orderly FIN close. Blocking and poll-driven
 *  -- every wait spins netif_poll_all()+hlt, exactly like arp_resolve()/ping --
 *  so RX is serviced on this thread while we wait. No retransmit queue, no
 *  congestion control, no out-of-order reassembly: a basic browser over slirp
 *  doesn't need them, and we keep transfers short and bounded by timeouts.
 * ===========================================================================*/

enum { CLOSED, SYN_SENT, ESTABLISHED, CLOSE_WAIT, FIN_WAIT, DONE };

#define RXBUF 65536                 /* per-connection receive ring */

struct tcp_conn {
    int           state;
    struct netif *nif;
    uint32_t      peer_ip;
    uint16_t      local_port, peer_port;
    uint32_t      snd_nxt;          /* next seq we will send          */
    uint32_t      snd_una;          /* oldest unacknowledged seq      */
    uint32_t      rcv_nxt;          /* next seq we expect from peer   */
    int           reset;            /* peer sent RST                  */
    int           fin_seen;         /* peer sent FIN                  */
    uint8_t      *rx;               /* receive ring (RXBUF bytes)     */
    uint32_t      rx_head, rx_tail; /* ring indices; count = head-tail*/
};

#define MAX_CONNS 4
static struct tcp_conn *conns[MAX_CONNS];
static uint16_t next_port = 49152;
static uint32_t isn_seed;

static uint32_t rx_count(struct tcp_conn *c) { return c->rx_head - c->rx_tail; }
static uint32_t rx_space(struct tcp_conn *c) { return RXBUF - rx_count(c); }

/* internet checksum over the TCP pseudo-header + segment */
static uint16_t tcp_checksum(const void *seg, uint16_t seglen, uint32_t src, uint32_t dst) {
    uint32_t sum = 0;
    uint32_t s = htonl(src), d = htonl(dst);
    const uint8_t *sb = (const uint8_t *)&s, *db = (const uint8_t *)&d;
    sum += (sb[0] << 8) | sb[1]; sum += (sb[2] << 8) | sb[3];
    sum += (db[0] << 8) | db[1]; sum += (db[2] << 8) | db[3];
    sum += IPPROTO_TCP;
    sum += seglen;
    const uint8_t *p = (const uint8_t *)seg;
    uint16_t i = 0;
    for (; i + 1 < seglen; i += 2) sum += (p[i] << 8) | p[i + 1];
    if (i < seglen) sum += p[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int tcp_xmit(struct tcp_conn *c, uint8_t flags, const void *data, uint16_t dlen) {
    uint8_t seg[20 + 1460];
    if (dlen > 1460) dlen = 1460;
    struct tcp_hdr *h = (struct tcp_hdr *)seg;
    h->src_port = htons(c->local_port);
    h->dst_port = htons(c->peer_port);
    h->seq      = htonl(c->snd_nxt);
    h->ack      = htonl(c->rcv_nxt);
    h->data_off = 5 << 4;
    h->flags    = flags;
    uint32_t win = rx_space(c); if (win > 0xFFFF) win = 0xFFFF;
    h->window   = htons((uint16_t)win);
    h->checksum = 0;
    h->urgent   = 0;
    if (dlen) memcpy(seg + 20, data, dlen);
    h->checksum = htons(tcp_checksum(seg, (uint16_t)(20 + dlen), net_ip, c->peer_ip));

    int r = ip_output(c->nif, c->peer_ip, IPPROTO_TCP, seg, (uint16_t)(20 + dlen));
    if (flags & TCP_SYN) c->snd_nxt++;
    if (flags & TCP_FIN) c->snd_nxt++;
    c->snd_nxt += dlen;
    return r;
}

void tcp_input(struct netif *nif, uint32_t src_ip, const uint8_t *pkt, uint16_t len) {
    if (len < (int)sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)pkt;
    uint16_t dport = ntohs(h->dst_port), sport = ntohs(h->src_port);

    struct tcp_conn *c = 0;
    for (int i = 0; i < MAX_CONNS; i++)
        if (conns[i] && conns[i]->local_port == dport &&
            conns[i]->peer_port == sport && conns[i]->peer_ip == src_ip) { c = conns[i]; break; }
    if (!c) return;
    (void)nif;

    uint16_t thl = (uint16_t)((h->data_off >> 4) * 4);
    if (thl < 20 || thl > len) return;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    uint8_t  fl  = h->flags;
    const uint8_t *data = pkt + thl;
    uint16_t dlen = (uint16_t)(len - thl);

    if (fl & TCP_RST) { c->reset = 1; c->state = DONE; return; }

    if (c->state == SYN_SENT) {
        if ((fl & TCP_SYN) && (fl & TCP_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->state   = ESTABLISHED;
            tcp_xmit(c, TCP_ACK, 0, 0);               /* complete handshake */
        }
        return;
    }

    if (fl & TCP_ACK) { if ((int32_t)(ack - c->snd_una) > 0) c->snd_una = ack; }

    /* accept in-order data only; copy what fits, advance ACK by what we took */
    if (dlen && seq == c->rcv_nxt) {
        uint32_t take = dlen; uint32_t sp = rx_space(c);
        if (take > sp) take = sp;
        for (uint32_t i = 0; i < take; i++) c->rx[(c->rx_head++) % RXBUF] = data[i];
        c->rcv_nxt += take;
        tcp_xmit(c, TCP_ACK, 0, 0);
    } else if (dlen) {
        tcp_xmit(c, TCP_ACK, 0, 0);                   /* out of order -> dup ACK */
    }

    if ((fl & TCP_FIN) && seq + dlen == c->rcv_nxt) {
        c->rcv_nxt++;                                 /* FIN consumes one seq */
        c->fin_seen = 1;
        tcp_xmit(c, TCP_ACK, 0, 0);
        if (c->state == ESTABLISHED) c->state = CLOSE_WAIT;
        else if (c->state == FIN_WAIT) c->state = DONE;
    }
}

static void pump(uint32_t ms) {
    uint64_t start = pit_ticks();
    do { netif_poll_all(); __asm__ volatile("hlt"); } while (pit_ticks() - start < ms);
}

struct tcp_conn *tcp_connect(uint32_t dst_ip, uint16_t dport, uint32_t timeout_ms) {
    struct netif *nif = netif_default();
    if (!nif) return 0;

    int slot = -1;
    for (int i = 0; i < MAX_CONNS; i++) if (!conns[i]) { slot = i; break; }
    if (slot < 0) return 0;

    struct tcp_conn *c = (struct tcp_conn *)kmalloc(sizeof(*c));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));
    c->rx = (uint8_t *)kmalloc(RXBUF);
    if (!c->rx) { kfree(c); return 0; }
    c->nif        = nif;
    c->peer_ip    = dst_ip;
    c->peer_port  = dport;
    c->local_port = next_port++; if (next_port == 0) next_port = 49152;
    isn_seed += (uint32_t)pit_ticks() * 2654435761u + 0x9E3779B9u;
    c->snd_nxt = c->snd_una = isn_seed;
    c->state   = SYN_SENT;
    conns[slot] = c;

    /* resolve next hop once up front so the SYN isn't dropped behind ARP */
    uint32_t nexthop = ((dst_ip & net_mask) == (net_ip & net_mask)) ? dst_ip : net_gw;
    uint8_t mac[6];
    if (!arp_resolve(nif, nexthop, mac, 1000)) { tcp_close(c); return 0; }

    uint64_t start = pit_ticks();
    uint64_t last  = 0;
    while (pit_ticks() - start < timeout_ms) {
        if (pit_ticks() - last >= 500 && c->state == SYN_SENT) {
            last = pit_ticks();
            c->snd_nxt = c->snd_una;                  /* rewind for SYN retransmit */
            tcp_xmit(c, TCP_SYN, 0, 0);
        }
        netif_poll_all();
        if (c->state == ESTABLISHED) return c;
        if (c->state == DONE || c->reset) break;
        __asm__ volatile("hlt");
    }
    tcp_close(c);
    return 0;
}

int tcp_send(struct tcp_conn *c, const void *data, uint32_t len) {
    if (!c || c->state != ESTABLISHED) return -1;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off; if (chunk > 1460) chunk = 1460;
        uint32_t want_una = c->snd_nxt + chunk;
        tcp_xmit(c, TCP_PSH | TCP_ACK, p + off, (uint16_t)chunk);
        /* wait for the ACK (with a couple of retransmits) */
        uint64_t start = pit_ticks(), last = pit_ticks();
        while (c->snd_una != want_una && pit_ticks() - start < 3000) {
            if (pit_ticks() - last >= 600) {
                last = pit_ticks();
                c->snd_nxt -= chunk;                  /* rewind and resend */
                tcp_xmit(c, TCP_PSH | TCP_ACK, p + off, (uint16_t)chunk);
            }
            netif_poll_all();
            if (c->reset) return -1;
            __asm__ volatile("hlt");
        }
        if (c->snd_una != want_una) return -1;
        off += chunk;
    }
    return (int)len;
}

int tcp_recv(struct tcp_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms) {
    if (!c) return -1;
    uint64_t start = pit_ticks();
    for (;;) {
        if (rx_count(c) > 0) {
            uint32_t n = rx_count(c); if (n > cap) n = cap;
            uint8_t *o = (uint8_t *)buf;
            for (uint32_t i = 0; i < n; i++) o[i] = c->rx[(c->rx_tail++) % RXBUF];
            return (int)n;
        }
        if (c->reset) return -1;
        if (c->fin_seen) return 0;                    /* peer closed, ring drained */
        if (pit_ticks() - start >= timeout_ms) return -1;
        netif_poll_all();
        __asm__ volatile("hlt");
    }
}

void tcp_close(struct tcp_conn *c) {
    if (!c) return;
    if (c->state == ESTABLISHED || c->state == CLOSE_WAIT) {
        int waslast = (c->state == CLOSE_WAIT);
        tcp_xmit(c, TCP_FIN | TCP_ACK, 0, 0);
        c->state = waslast ? DONE : FIN_WAIT;
        pump(400);                                    /* brief drain for the ACK/FIN */
    }
    for (int i = 0; i < MAX_CONNS; i++) if (conns[i] == c) conns[i] = 0;
    if (c->rx) kfree(c->rx);
    kfree(c);
}
