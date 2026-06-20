#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Packet-filtering firewall. Rules are evaluated in order, first match wins;
 *  if nothing matches, the default policy applies. The IPv4 layer calls
 *  fw_check() on every datagram, inbound and outbound, so a BLOCK verdict
 *  actually drops the packet -- this is a real filter, not advisory state.
 * ===========================================================================*/
#define FW_IN   1
#define FW_OUT  2
#define FW_ANY  3

/* dir is FW_IN or FW_OUT; proto is IPPROTO_*; peer is the remote IP (host
 * order): the source for inbound, the destination for outbound. port is the
 * remote port (host order, 0 if not TCP/UDP). Returns 1 to pass, 0 to drop. */
int  fw_check(int dir, uint8_t proto, uint32_t peer, uint16_t port);

int  fw_enabled(void);
void fw_set_enabled(int on);
void fw_set_default(int allow);
int  fw_get_default(void);

/* Parse a rule from tokens (after "firewall add"). Returns 0 ok, -1 bad
 * syntax, -2 table full. raw is the original rule text for `list`. */
int  fw_add(int ntok, char **tok, const char *raw);
void fw_clear(void);
int  fw_count(void);
const char *fw_rule_text(int i);

void fw_stats(uint32_t *passed, uint32_t *blocked);
