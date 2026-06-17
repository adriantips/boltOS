#pragma once
#include <stdint.h>
#include "netif.h"

/* ===========================================================================
 *  Minimal IPv4 stack: Ethernet / ARP / IPv4 / ICMP / UDP.
 *  IP addresses are carried as uint32_t in HOST order; conversion to network
 *  order happens at the wire boundary. x86 is little-endian.
 * ===========================================================================*/
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8) |
           ((x >> 8) & 0xFF00u) | ((x >> 24) & 0xFFu);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806
#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define ETH_HDR_LEN    14
#define ETH_MIN_FRAME  60          /* min frame sans FCS; pad shorter ones      */

#define ip4(a,b,c,d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                      ((uint32_t)(c) << 8)  |  (uint32_t)(d))

struct eth_hdr { uint8_t dst[6]; uint8_t src[6]; uint16_t type; } __attribute__((packed));

struct arp_hdr {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[6]; uint8_t spa[4];
    uint8_t  tha[6]; uint8_t tpa[4];
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ver_ihl;          /* 0x45 */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;              /* network order */
    uint32_t dst;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;
} __attribute__((packed));

struct udp_hdr { uint16_t src_port, dst_port, len, checksum; } __attribute__((packed));

struct tcp_hdr {
    uint16_t src_port, dst_port;
    uint32_t seq, ack;
    uint8_t  data_off;         /* high nibble: header length / 4 */
    uint8_t  flags;            /* FIN|SYN|RST|PSH|ACK ...        */
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* Static interface config (host order). Defaults match QEMU user-net (slirp). */
extern uint32_t net_ip, net_mask, net_gw, net_dns;

uint16_t net_checksum(const void *data, uint32_t len);
uint32_t net_parse_ipv4(const char *s, int *ok);   /* "a.b.c.d" -> host order  */

/* Ethernet */
void eth_input(struct netif *nif, const uint8_t *frame, uint16_t len);
int  eth_send(struct netif *nif, const uint8_t dst[6], uint16_t ethertype,
              const void *payload, uint16_t len);

/* ARP */
void arp_input(struct netif *nif, const uint8_t *frame, uint16_t len);
int  arp_lookup(uint32_t ip, uint8_t mac_out[6]);   /* 1 = known */
void arp_cache_put(uint32_t ip, const uint8_t mac[6]);
void arp_request(struct netif *nif, uint32_t ip);
int  arp_resolve(struct netif *nif, uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms);

/* IPv4 */
void ip_input(struct netif *nif, const uint8_t *pkt, uint16_t len,
              const struct eth_hdr *eth);
int  ip_output(struct netif *nif, uint32_t dst_ip, uint8_t proto,
               const void *payload, uint16_t len);

/* ICMP */
void icmp_input(struct netif *nif, uint32_t src_ip, const uint8_t *pkt, uint16_t len);
int  icmp_echo_send(struct netif *nif, uint32_t dst_ip, uint16_t id, uint16_t seq);
int  icmp_wait_reply(uint16_t id, uint16_t seq, uint32_t timeout_ms); /* 1 = got */

/* UDP */
void udp_input(struct netif *nif, uint32_t src_ip, const uint8_t *pkt, uint16_t len);
int  udp_send(struct netif *nif, uint32_t dst_ip, uint16_t sport, uint16_t dport,
              const void *data, uint16_t len);
/* Bind a callback to a local UDP port (one per port). Used by the DNS resolver. */
typedef void (*udp_listener_fn)(uint32_t src_ip, uint16_t sport,
                                const uint8_t *data, uint16_t len);
void udp_listen(uint16_t port, udp_listener_fn fn);
void udp_unlisten(uint16_t port);

/* TCP (active-open client). Blocking, poll-driven -- same model as arp_resolve.
 * Connections are opaque; one heap allocation per connection. */
struct tcp_conn;
void tcp_input(struct netif *nif, uint32_t src_ip, const uint8_t *pkt, uint16_t len);
struct tcp_conn *tcp_connect(uint32_t dst_ip, uint16_t dport, uint32_t timeout_ms);
int  tcp_send(struct tcp_conn *c, const void *data, uint32_t len);
/* Blocks until some bytes arrive, the peer closes (returns 0), or timeout (-1). */
int  tcp_recv(struct tcp_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms);
void tcp_close(struct tcp_conn *c);

/* DNS: resolve a hostname to an IPv4 address (host order). 1 = ok, 0 = failed. */
int  dns_resolve(const char *host, uint32_t *ip_out, uint32_t timeout_ms);
