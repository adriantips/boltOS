#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Generic network-interface abstraction.
 *
 *  A driver fills in a struct netif (MAC, send op, optional poll op) and calls
 *  netif_register(). Received frames go up via netif_rx(), which hands them to
 *  the input handler registered by the protocol stack (P3). This is the single
 *  seam every link driver -- wired NIC today, 802.11 later -- sits behind.
 * ===========================================================================*/
#define ETH_ALEN   6
#define NETIF_MTU  1500
#define ETH_FRAME_MAX 1522     /* 14 hdr + 1500 payload + 4 FCS + slack */

struct netif {
    char     name[8];
    uint8_t  mac[ETH_ALEN];
    int      link_up;
    void    *drv;                                          /* driver private    */
    int    (*send)(struct netif *nif, const void *frame, uint16_t len);
    void   (*poll)(struct netif *nif);                     /* optional RX poll  */

    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
    struct netif *next;
};

void          net_init(void);                 /* init core, then probe drivers */
void          netif_register(struct netif *nif);
struct netif *netif_default(void);
struct netif *netif_list(void);
int           netif_send(struct netif *nif, const void *frame, uint16_t len);
void          netif_poll_all(void);

/* Driver -> stack: deliver a received Ethernet frame. Updates stats and calls
 * the registered input handler; if none, the frame is counted and dropped. */
void netif_rx(struct netif *nif, const uint8_t *frame, uint16_t len);

/* The protocol stack (P3) installs its frame-input handler here. */
typedef void (*netif_input_fn)(struct netif *nif, const uint8_t *frame, uint16_t len);
void netif_set_input(netif_input_fn fn);
