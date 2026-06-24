#include <stdint.h>
#include "dhcp.h"
#include "net.h"
#include "netif.h"
#include "pit.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  DHCP client (RFC 2131). Blocking, poll-driven, same model as the ARP/DNS
 *  resolvers: send, then spin netif_poll_all() until the listener flags an
 *  answer or the timeout expires.
 * ===========================================================================*/

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC       0x63825363u

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5

struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} __attribute__((packed));

static volatile int      got_offer, got_ack;
static uint32_t          offered_ip, server_id, lease_secs, sub_mask, router_ip, dns_ip;
static uint32_t          cur_xid;

uint32_t dhcp_leased_ip(void)  { return offered_ip; }
uint32_t dhcp_server_id(void)  { return server_id; }
uint32_t dhcp_lease_secs(void) { return lease_secs; }

static uint32_t rdtsc32(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ (hi << 1);
}

/* Append a DHCP option; returns new write pointer. */
static uint8_t *opt(uint8_t *p, uint8_t code, uint8_t len, const void *val) {
    *p++ = code; *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

static int build_msg(struct dhcp_msg *m, uint8_t type, const uint8_t mac[6]) {
    memset(m, 0, sizeof(*m));
    m->op = 1; m->htype = 1; m->hlen = 6;
    m->xid = cur_xid;
    m->flags = htons(0x8000);                   /* ask server to broadcast reply */
    memcpy(m->chaddr, mac, 6);
    uint8_t *p = m->options;
    *(uint32_t *)p = htonl(DHCP_MAGIC); p += 4;
    p = opt(p, 53, 1, &type);                   /* message type */
    if (type == DHCPREQUEST) {
        uint32_t ip = htonl(offered_ip);
        uint32_t sid = htonl(server_id);
        p = opt(p, 50, 4, &ip);                 /* requested IP */
        p = opt(p, 54, 4, &sid);                /* server identifier */
    }
    uint8_t params[] = {1, 3, 6, 15, 51};       /* mask, router, dns, domain, lease */
    p = opt(p, 55, sizeof(params), params);
    *p++ = 255;                                 /* end */
    return (int)((uint8_t *)p - (uint8_t *)m);
}

/* parse options, return DHCP message type (0 if none) */
static uint8_t parse_reply(const struct dhcp_msg *m, uint16_t len) {
    if (len < 240) return 0;
    if (m->xid != cur_xid) return 0;
    if (ntohl(*(const uint32_t *)m->options) != DHCP_MAGIC) return 0;

    uint8_t mtype = 0;
    const uint8_t *p = m->options + 4;
    const uint8_t *end = (const uint8_t *)m + len;
    offered_ip = ntohl(m->yiaddr);
    while (p < end && *p != 255) {
        uint8_t code = *p++;
        if (code == 0) continue;
        uint8_t olen = *p++;
        if (p + olen > end) break;
        switch (code) {
        case 53: mtype = p[0]; break;
        case 1:  sub_mask = ntohl(*(const uint32_t *)p); break;
        case 3:  router_ip = ntohl(*(const uint32_t *)p); break;
        case 6:  dns_ip = ntohl(*(const uint32_t *)p); break;
        case 54: server_id = ntohl(*(const uint32_t *)p); break;
        case 51: lease_secs = ntohl(*(const uint32_t *)p); break;
        }
        p += olen;
    }
    return mtype;
}

static void on_dhcp(uint32_t src_ip, uint16_t sport, const uint8_t *data, uint16_t len) {
    (void)src_ip; (void)sport;
    uint8_t t = parse_reply((const struct dhcp_msg *)data, len);
    if (t == DHCPOFFER) got_offer = 1;
    else if (t == DHCPACK) got_ack = 1;
}

static int wait_flag(volatile int *flag, uint32_t timeout_ms) {
    uint64_t end = pit_ticks() + timeout_ms;
    while (pit_ticks() < end) {
        netif_poll_all();
        if (*flag) return 0;
    }
    return -1;
}

int dhcp_configure(uint32_t timeout_ms) {
    struct netif *nif = netif_default();
    if (!nif) return -1;

    /* save static config so we can roll back on failure */
    uint32_t save_ip = net_ip, save_mask = net_mask, save_gw = net_gw, save_dns = net_dns;
    got_offer = got_ack = 0;
    offered_ip = server_id = lease_secs = sub_mask = router_ip = dns_ip = 0;
    cur_xid = rdtsc32();

    net_ip = 0;                                 /* unconfigured during discovery */
    udp_listen(DHCP_CLIENT_PORT, on_dhcp);

    struct dhcp_msg m;
    int len = build_msg(&m, DHCPDISCOVER, nif->mac);
    udp_send(nif, 0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &m, (uint16_t)len);

    if (wait_flag(&got_offer, timeout_ms) != 0) {
        udp_unlisten(DHCP_CLIENT_PORT);
        net_ip = save_ip; net_mask = save_mask; net_gw = save_gw; net_dns = save_dns;
        kprintf("[dhcp] no OFFER; keeping static %d.%d.%d.%d\n",
                (save_ip>>24)&0xff,(save_ip>>16)&0xff,(save_ip>>8)&0xff,save_ip&0xff);
        return -1;
    }

    len = build_msg(&m, DHCPREQUEST, nif->mac);
    udp_send(nif, 0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &m, (uint16_t)len);

    if (wait_flag(&got_ack, timeout_ms) != 0) {
        udp_unlisten(DHCP_CLIENT_PORT);
        net_ip = save_ip; net_mask = save_mask; net_gw = save_gw; net_dns = save_dns;
        kprintf("[dhcp] no ACK; keeping static config\n");
        return -1;
    }

    udp_unlisten(DHCP_CLIENT_PORT);

    /* install the lease */
    net_ip   = offered_ip;
    net_mask = sub_mask ? sub_mask : net_mask;
    net_gw   = router_ip ? router_ip : net_gw;
    net_dns  = dns_ip ? dns_ip : net_dns;

    kprintf("[ok] DHCP lease: %d.%d.%d.%d/%d.%d.%d.%d gw %d.%d.%d.%d dns %d.%d.%d.%d (%us)\n",
            (net_ip>>24)&0xff,(net_ip>>16)&0xff,(net_ip>>8)&0xff,net_ip&0xff,
            (net_mask>>24)&0xff,(net_mask>>16)&0xff,(net_mask>>8)&0xff,net_mask&0xff,
            (net_gw>>24)&0xff,(net_gw>>16)&0xff,(net_gw>>8)&0xff,net_gw&0xff,
            (net_dns>>24)&0xff,(net_dns>>16)&0xff,(net_dns>>8)&0xff,net_dns&0xff,
            lease_secs);
    return 0;
}
