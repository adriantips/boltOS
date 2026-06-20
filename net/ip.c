#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "firewall.h"
#include "string.h"

/* remote port (host order) carried in a TCP/UDP payload: outbound wants the
 * destination port (bytes 2..3), inbound wants the source port (bytes 0..1). */
static uint16_t peer_port(uint8_t proto, const uint8_t *l4, uint16_t l4len, int want_dst) {
    if (proto != IPPROTO_TCP && proto != IPPROTO_UDP) return 0;
    uint16_t off = want_dst ? 2 : 0;
    if (l4len < (uint16_t)(off + 2)) return 0;
    return (uint16_t)((l4[off] << 8) | l4[off + 1]);
}

static uint16_t ip_id;
static uint8_t  ippkt[1500];

int ip_output(struct netif *nif, uint32_t dst_ip, uint8_t proto,
              const void *payload, uint16_t len) {
    if (!nif) return -1;
    if (20 + len > sizeof(ippkt)) return -1;

    /* firewall: drop outbound datagrams a BLOCK rule rejects */
    if (!fw_check(FW_OUT, proto, dst_ip, peer_port(proto, payload, len, 1)))
        return -1;

    /* broadcast (limited 255.255.255.255 or directed subnet bcast) goes straight
     * to the Ethernet broadcast address with no ARP -- used by share discovery. */
    uint8_t mac[6];
    uint32_t subnet_bcast = net_ip | ~net_mask;
    if (dst_ip == 0xFFFFFFFFu || dst_ip == subnet_bcast) {
        for (int i = 0; i < 6; i++) mac[i] = 0xFF;
    } else {
        /* next hop: dst if on-link, else the gateway */
        uint32_t nexthop = ((dst_ip & net_mask) == (net_ip & net_mask)) ? dst_ip : net_gw;
        if (!arp_lookup(nexthop, mac)) { arp_request(nif, nexthop); return -1; }
    }

    struct ip_hdr *h = (struct ip_hdr *)ippkt;
    h->ver_ihl    = 0x45;
    h->tos        = 0;
    h->total_len  = htons((uint16_t)(20 + len));
    h->id         = htons(ip_id++);
    h->flags_frag = 0;
    h->ttl        = 64;
    h->proto      = proto;
    h->checksum   = 0;
    h->src        = htonl(net_ip);
    h->dst        = htonl(dst_ip);
    h->checksum   = htons(net_checksum(h, 20));
    memcpy(ippkt + 20, payload, len);

    return eth_send(nif, mac, ETHERTYPE_IP, ippkt, (uint16_t)(20 + len));
}

void ip_input(struct netif *nif, const uint8_t *pkt, uint16_t len,
              const struct eth_hdr *eth) {
    if (len < 20) return;
    const struct ip_hdr *h = (const struct ip_hdr *)pkt;
    if ((h->ver_ihl >> 4) != 4) return;
    uint16_t ihl = (uint16_t)((h->ver_ihl & 0x0F) * 4);
    if (ihl < 20 || ihl > len) return;

    uint16_t total = ntohs(h->total_len);
    if (total > len || total < ihl) return;
    uint32_t src = ntohl(h->src);

    /* learn the sender's MAC if it shares our subnet (avoids a round-trip ARP) */
    if ((src & net_mask) == (net_ip & net_mask))
        arp_cache_put(src, eth->src);

    const uint8_t *data = pkt + ihl;
    uint16_t dlen = (uint16_t)(total - ihl);

    /* firewall: drop inbound datagrams a BLOCK rule rejects */
    if (!fw_check(FW_IN, h->proto, src, peer_port(h->proto, data, dlen, 0)))
        return;

    switch (h->proto) {
    case IPPROTO_ICMP: icmp_input(nif, src, data, dlen); break;
    case IPPROTO_UDP:  udp_input(nif, src, data, dlen);  break;
    case IPPROTO_TCP:  tcp_input(nif, src, data, dlen);  break;
    default: break;
    }
}
