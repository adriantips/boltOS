#pragma once
#include <stdint.h>

/* DHCP client. dhcp_configure() runs a full DISCOVER/OFFER/REQUEST/ACK exchange
 * on the default interface and, on success, installs the leased IP, netmask,
 * gateway and DNS into the global net_* config. Returns 0 on success, <0 on
 * timeout/failure (caller keeps the existing static configuration). */
int  dhcp_configure(uint32_t timeout_ms);

/* Last lease details (host byte order), valid after a successful configure. */
uint32_t dhcp_leased_ip(void);
uint32_t dhcp_server_id(void);
uint32_t dhcp_lease_secs(void);
