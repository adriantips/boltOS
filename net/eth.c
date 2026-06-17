#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "string.h"

/* host config -- defaults to QEMU/VBox user-net (10.0.2.15/24 via 10.0.2.2,
 * DNS proxy at 10.0.2.3). slirp NATs these straight onto the host network. */
uint32_t net_ip   = ip4(10, 0, 2, 15);
uint32_t net_mask = ip4(255, 255, 255, 0);
uint32_t net_gw   = ip4(10, 0, 2, 2);
uint32_t net_dns  = ip4(10, 0, 2, 3);

uint16_t net_checksum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint32_t)(p[0] << 8 | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t net_parse_ipv4(const char *s, int *ok) {
    uint32_t b[4] = {0,0,0,0};
    int part = 0, digits = 0, val = 0;
    for (; ; s++) {
        if (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); digits++; if (val > 255) goto bad; }
        else if (*s == '.' || *s == 0) {
            if (!digits || part > 3) goto bad;
            b[part++] = (uint32_t)val; val = 0; digits = 0;
            if (*s == 0) break;
        } else goto bad;
    }
    if (part != 4) goto bad;
    if (ok) *ok = 1;
    return ip4(b[0], b[1], b[2], b[3]);
bad:
    if (ok) *ok = 0;
    return 0;
}

/* one TX scratch frame; sends are serialized (shell-driven) for now */
static uint8_t txframe[ETH_FRAME_MAX];

int eth_send(struct netif *nif, const uint8_t dst[6], uint16_t ethertype,
             const void *payload, uint16_t len) {
    if (!nif) return -1;
    uint16_t total = (uint16_t)(ETH_HDR_LEN + len);
    if (total > sizeof(txframe)) return -1;

    struct eth_hdr *e = (struct eth_hdr *)txframe;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, nif->mac, 6);
    e->type = htons(ethertype);
    memcpy(txframe + ETH_HDR_LEN, payload, len);

    if (total < ETH_MIN_FRAME) {                 /* pad runt frames */
        memset(txframe + total, 0, ETH_MIN_FRAME - total);
        total = ETH_MIN_FRAME;
    }
    return netif_send(nif, txframe, total);
}

void eth_input(struct netif *nif, const uint8_t *frame, uint16_t len) {
    if (len < ETH_HDR_LEN) return;
    const struct eth_hdr *e = (const struct eth_hdr *)frame;
    uint16_t type = ntohs(e->type);
    if (type == ETHERTYPE_ARP)
        arp_input(nif, frame, len);
    else if (type == ETHERTYPE_IP)
        ip_input(nif, frame + ETH_HDR_LEN, (uint16_t)(len - ETH_HDR_LEN), e);
}
