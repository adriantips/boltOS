#pragma once
#include <stdint.h>
#include "netif.h"

/* ===========================================================================
 *  IEEE 802.11 MAC structures + softMAC helpers (P4).
 *
 *  Enough of the 802.11 MAC for a softMAC driver (ath9k/P5) to: hand received
 *  data frames up as Ethernet (the IPv4 stack already speaks struct eth_hdr),
 *  parse beacons into a scan list, and build the management frames needed to
 *  join an open/WPA2 BSS. These helpers are link-independent -- no hardware or
 *  register knowledge lives here, only frame format -- so they are unit-testable
 *  before any radio exists.
 * ===========================================================================*/
#define IEEE80211_ADDR_LEN 6

/* Frame Control (little-endian on the wire): bits 0-1 version, 2-3 type,
 * 4-7 subtype, then per-bit flags in the high byte. */
#define FC_TYPE_MASK    0x000C
#define FC_SUBTYPE_MASK 0x00F0
#define FC_TYPE_MGMT    0x0000
#define FC_TYPE_CTRL    0x0004
#define FC_TYPE_DATA    0x0008
#define FC_TODS         0x0100
#define FC_FROMDS       0x0200
#define FC_RETRY        0x0800
#define FC_PROTECTED    0x4000

/* Management subtypes, pre-shifted into the subtype field (bits 4-7). */
#define MGMT_ASSOC_REQ  0x0000
#define MGMT_ASSOC_RESP 0x0010
#define MGMT_PROBE_REQ  0x0040
#define MGMT_PROBE_RESP 0x0050
#define MGMT_BEACON     0x0080
#define MGMT_DISASSOC   0x00A0
#define MGMT_AUTH       0x00B0
#define MGMT_DEAUTH     0x00C0

/* Generic 3-address MAC header (the common data/management layout). */
struct ieee80211_hdr {
    uint16_t frame_control;
    uint16_t duration_id;
    uint8_t  addr1[IEEE80211_ADDR_LEN];   /* RA / DA   */
    uint8_t  addr2[IEEE80211_ADDR_LEN];   /* TA / SA   */
    uint8_t  addr3[IEEE80211_ADDR_LEN];   /* BSSID     */
    uint16_t seq_ctrl;
} __attribute__((packed));

/* Fixed body of a beacon / probe-response, followed by tagged info elements. */
struct ieee80211_beacon {
    uint64_t timestamp;
    uint16_t beacon_interval;
    uint16_t capability;
} __attribute__((packed));
#define CAP_PRIVACY 0x0010                /* capability bit: encrypted BSS */

/* Tagged information element (TLV). */
struct ieee80211_ie { uint8_t id, len; uint8_t data[]; } __attribute__((packed));
#define IE_SSID    0
#define IE_RATES   1
#define IE_DSPARAM 3                       /* one byte: channel */
#define IE_RSN     48                      /* WPA2 */

/* LLC/SNAP header prefixing an 802.11 data payload; its ethertype maps directly
 * onto the Ethernet II type field. */
struct llc_snap {
    uint8_t  dsap, ssap, control;
    uint8_t  oui[3];
    uint16_t ethertype;
} __attribute__((packed));
#define LLC_SNAP_AA 0xAA                   /* dsap==ssap==0xAA, control==0x03 */

/* One discovered BSS (a scan-result row). */
struct wifi_bss {
    uint8_t  bssid[IEEE80211_ADDR_LEN];
    char     ssid[33];
    uint8_t  channel;
    int8_t   rssi;
    uint16_t capability;
    int      privacy;
};

/* SoftMAC association state machine. */
enum wifi_state { WIFI_IDLE, WIFI_SCANNING, WIFI_AUTH, WIFI_ASSOC, WIFI_RUN };

/* ---- MAC-layer helpers (net/wifi.c) ---- */

/* Convert a received 802.11 data frame (hdr + LLC/SNAP + payload) into an
 * Ethernet II frame in out. Returns the eth frame length, or -1 if the frame is
 * not a forwardable 3-address data frame. */
int ieee80211_data_to_eth(const uint8_t *frame, uint16_t len,
                          uint8_t *out, uint16_t out_cap);

/* Parse a beacon / probe-response into *bss. Returns 0 on success, -1 if the
 * frame is not a beacon/probe-response or is too short. */
int ieee80211_parse_beacon(const uint8_t *frame, uint16_t len, struct wifi_bss *bss);

/* Build a probe-request (broadcast DA/BSSID) into out; ssid NULL/"" = wildcard.
 * Returns the frame length. out must hold >= 38 + len(ssid) bytes. */
int ieee80211_build_probe_req(uint8_t *out, const uint8_t src[6], const char *ssid);

const char *wifi_state_name(enum wifi_state s);
