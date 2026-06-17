#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "string.h"

/* Minimal UDP: send + receive dispatch. No socket layer yet -- received
 * datagrams are counted and dropped until a listener API lands. The IPv4 UDP
 * checksum is optional, so we send 0 (skip) for simplicity. */
static uint8_t udppkt[1500];

int udp_send(struct netif *nif, uint32_t dst_ip, uint16_t sport, uint16_t dport,
             const void *data, uint16_t len) {
    if (sizeof(struct udp_hdr) + len > sizeof(udppkt)) return -1;
    struct udp_hdr *h = (struct udp_hdr *)udppkt;
    h->src_port = htons(sport);
    h->dst_port = htons(dport);
    h->len      = htons((uint16_t)(sizeof(*h) + len));
    h->checksum = 0;     /* optional for IPv4 */
    memcpy(udppkt + sizeof(*h), data, len);
    return ip_output(nif, dst_ip, IPPROTO_UDP, udppkt, (uint16_t)(sizeof(*h) + len));
}

/* Port -> listener demux. Small table; the DNS resolver binds one ephemeral
 * port at a time. */
#define UDP_LISTENERS 8
static struct { uint16_t port; udp_listener_fn fn; } listeners[UDP_LISTENERS];

void udp_listen(uint16_t port, udp_listener_fn fn) {
    for (int i = 0; i < UDP_LISTENERS; i++)
        if (listeners[i].port == 0) { listeners[i].port = port; listeners[i].fn = fn; return; }
}
void udp_unlisten(uint16_t port) {
    for (int i = 0; i < UDP_LISTENERS; i++)
        if (listeners[i].port == port) { listeners[i].port = 0; listeners[i].fn = 0; return; }
}

void udp_input(struct netif *nif, uint32_t src_ip, const uint8_t *pkt, uint16_t len) {
    (void)nif;
    if (len < (int)sizeof(struct udp_hdr)) return;
    const struct udp_hdr *h = (const struct udp_hdr *)pkt;
    uint16_t dport = ntohs(h->dst_port);
    uint16_t ulen  = ntohs(h->len);
    if (ulen < sizeof(*h) || ulen > len) ulen = len;
    const uint8_t *data = pkt + sizeof(*h);
    uint16_t dlen = (uint16_t)(ulen - sizeof(*h));
    for (int i = 0; i < UDP_LISTENERS; i++)
        if (listeners[i].port == dport && listeners[i].fn) {
            listeners[i].fn(src_ip, ntohs(h->src_port), data, dlen);
            return;
        }
}
