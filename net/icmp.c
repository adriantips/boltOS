#include <stdint.h>
#include "net.h"
#include "netif.h"
#include "pit.h"
#include "string.h"

#define ICMP_PAYLOAD 32

/* last echo reply we saw, for icmp_wait_reply() */
static volatile uint16_t reply_id, reply_seq;
static volatile int      reply_got;

int icmp_echo_send(struct netif *nif, uint32_t dst_ip, uint16_t id, uint16_t seq) {
    uint8_t buf[sizeof(struct icmp_hdr) + ICMP_PAYLOAD];
    struct icmp_hdr *h = (struct icmp_hdr *)buf;
    h->type = ICMP_ECHO_REQUEST; h->code = 0; h->checksum = 0;
    h->id = htons(id); h->seq = htons(seq);
    for (int i = 0; i < ICMP_PAYLOAD; i++) buf[sizeof(*h) + i] = (uint8_t)('a' + (i % 26));
    h->checksum = htons(net_checksum(buf, sizeof(buf)));
    return ip_output(nif, dst_ip, IPPROTO_ICMP, buf, sizeof(buf));
}

void icmp_input(struct netif *nif, uint32_t src_ip, const uint8_t *pkt, uint16_t len) {
    if (len < (int)sizeof(struct icmp_hdr)) return;
    const struct icmp_hdr *h = (const struct icmp_hdr *)pkt;

    if (h->type == ICMP_ECHO_REPLY) {
        reply_id  = ntohs(h->id);
        reply_seq = ntohs(h->seq);
        reply_got = 1;
    } else if (h->type == ICMP_ECHO_REQUEST) {
        /* echo it back */
        uint8_t buf[1500];
        if (len > sizeof(buf)) return;
        memcpy(buf, pkt, len);
        struct icmp_hdr *r = (struct icmp_hdr *)buf;
        r->type = ICMP_ECHO_REPLY; r->checksum = 0;
        r->checksum = htons(net_checksum(buf, len));
        ip_output(nif, src_ip, IPPROTO_ICMP, buf, len);
    }
}

int icmp_wait_reply(uint16_t id, uint16_t seq, uint32_t timeout_ms) {
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < timeout_ms) {
        netif_poll_all();
        if (reply_got && reply_id == id && reply_seq == seq) { reply_got = 0; return 1; }
        __asm__ volatile("hlt");
    }
    return 0;
}
