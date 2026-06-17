#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "pit.h"
#include "string.h"

#define ARP_CACHE 16
struct arp_entry { uint32_t ip; uint8_t mac[6]; int valid; };
static struct arp_entry cache[ARP_CACHE];

static uint32_t ip_of(const uint8_t b[4]) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}
static void ip_bytes(uint32_t ip, uint8_t b[4]) {
    b[0] = ip >> 24; b[1] = ip >> 16; b[2] = ip >> 8; b[3] = ip;
}

int arp_lookup(uint32_t ip, uint8_t mac_out[6]) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (cache[i].valid && cache[i].ip == ip) { memcpy(mac_out, cache[i].mac, 6); return 1; }
    return 0;
}

void arp_cache_put(uint32_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (cache[i].valid && cache[i].ip == ip) { memcpy(cache[i].mac, mac, 6); return; }
    for (int i = 0; i < ARP_CACHE; i++)
        if (!cache[i].valid) { cache[i].valid = 1; cache[i].ip = ip; memcpy(cache[i].mac, mac, 6); return; }
    cache[0].ip = ip; memcpy(cache[0].mac, mac, 6);   /* evict slot 0 */
}

void arp_request(struct netif *nif, uint32_t ip) {
    struct arp_hdr a;
    a.htype = htons(1); a.ptype = htons(ETHERTYPE_IP);
    a.hlen = 6; a.plen = 4; a.oper = htons(1);
    memcpy(a.sha, nif->mac, 6);
    ip_bytes(net_ip, a.spa);
    memset(a.tha, 0, 6);
    ip_bytes(ip, a.tpa);
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eth_send(nif, bcast, ETHERTYPE_ARP, &a, sizeof(a));
}

void arp_input(struct netif *nif, const uint8_t *frame, uint16_t len) {
    if (len < ETH_HDR_LEN + (int)sizeof(struct arp_hdr)) return;
    const struct arp_hdr *a = (const struct arp_hdr *)(frame + ETH_HDR_LEN);
    if (ntohs(a->ptype) != ETHERTYPE_IP) return;

    uint32_t spa = ip_of(a->spa), tpa = ip_of(a->tpa);
    arp_cache_put(spa, a->sha);                 /* learn the sender */

    if (ntohs(a->oper) == 1 && tpa == net_ip) { /* request for us -> reply */
        struct arp_hdr r;
        r.htype = htons(1); r.ptype = htons(ETHERTYPE_IP);
        r.hlen = 6; r.plen = 4; r.oper = htons(2);
        memcpy(r.sha, nif->mac, 6); ip_bytes(net_ip, r.spa);
        memcpy(r.tha, a->sha, 6);   ip_bytes(spa, r.tpa);
        eth_send(nif, a->sha, ETHERTYPE_ARP, &r, sizeof(r));
    }
}

int arp_resolve(struct netif *nif, uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms) {
    if (arp_lookup(ip, mac_out)) return 1;
    arp_request(nif, ip);
    uint64_t start = pit_ticks();               /* PIT runs at 1000 Hz -> ms */
    while (pit_ticks() - start < timeout_ms) {
        netif_poll_all();
        if (arp_lookup(ip, mac_out)) return 1;
        __asm__ volatile("hlt");
    }
    return 0;
}
