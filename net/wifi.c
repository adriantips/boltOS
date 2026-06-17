#include <stdint.h>
#include "wifi.h"
#include "net.h"        /* struct eth_hdr, ETH_HDR_LEN */
#include "string.h"

/* ===========================================================================
 *  802.11 MAC-layer helpers. Pure frame-format code: a softMAC driver feeds raw
 *  802.11 frames in and gets Ethernet frames / parsed BSS info out. No radio,
 *  register, or DMA knowledge here -- that lives in the per-chip driver (P5).
 * ===========================================================================*/

int ieee80211_data_to_eth(const uint8_t *frame, uint16_t len,
                          uint8_t *out, uint16_t out_cap) {
    if (len < sizeof(struct ieee80211_hdr) + sizeof(struct llc_snap)) return -1;
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)frame;
    uint16_t fc = h->frame_control;
    if ((fc & FC_TYPE_MASK) != FC_TYPE_DATA) return -1;

    /* 3-address forms map ToDS/FromDS to which header address is DA vs SA. */
    const uint8_t *da, *sa;
    switch (fc & (FC_TODS | FC_FROMDS)) {
    case 0:            da = h->addr1; sa = h->addr2; break;   /* IBSS            */
    case FC_FROMDS:    da = h->addr1; sa = h->addr3; break;   /* AP -> STA       */
    case FC_TODS:      da = h->addr3; sa = h->addr2; break;   /* STA -> AP       */
    default:           return -1;                            /* 4-addr WDS: skip */
    }

    const struct llc_snap *snap =
        (const struct llc_snap *)(frame + sizeof(struct ieee80211_hdr));
    if (snap->dsap != LLC_SNAP_AA || snap->ssap != LLC_SNAP_AA) return -1;

    const uint8_t *payload = (const uint8_t *)snap + sizeof(struct llc_snap);
    uint16_t paylen = (uint16_t)(len - sizeof(struct ieee80211_hdr)
                                     - sizeof(struct llc_snap));
    if ((uint32_t)ETH_HDR_LEN + paylen > out_cap) return -1;

    struct eth_hdr *e = (struct eth_hdr *)out;
    memcpy(e->dst, da, IEEE80211_ADDR_LEN);
    memcpy(e->src, sa, IEEE80211_ADDR_LEN);
    e->type = snap->ethertype;            /* SNAP ethertype is already net order */
    memcpy(out + ETH_HDR_LEN, payload, paylen);
    return ETH_HDR_LEN + paylen;
}

int ieee80211_parse_beacon(const uint8_t *frame, uint16_t len, struct wifi_bss *bss) {
    uint32_t hdrlen = sizeof(struct ieee80211_hdr);
    if (len < hdrlen + sizeof(struct ieee80211_beacon)) return -1;
    const struct ieee80211_hdr *h = (const struct ieee80211_hdr *)frame;
    uint16_t fc = h->frame_control;
    if ((fc & FC_TYPE_MASK) != FC_TYPE_MGMT) return -1;
    uint16_t st = fc & FC_SUBTYPE_MASK;
    if (st != MGMT_BEACON && st != MGMT_PROBE_RESP) return -1;

    memset(bss, 0, sizeof(*bss));
    memcpy(bss->bssid, h->addr3, IEEE80211_ADDR_LEN);
    const struct ieee80211_beacon *b =
        (const struct ieee80211_beacon *)(frame + hdrlen);
    bss->capability = b->capability;
    bss->privacy    = (b->capability & CAP_PRIVACY) ? 1 : 0;

    /* Walk tagged IEs for SSID + channel; ignore the rest. */
    uint32_t off = hdrlen + sizeof(struct ieee80211_beacon);
    while (off + 2 <= len) {
        uint8_t id = frame[off], ilen = frame[off + 1];
        if (off + 2u + ilen > len) break;
        const uint8_t *d = frame + off + 2;
        if (id == IE_SSID && ilen <= 32) { memcpy(bss->ssid, d, ilen); bss->ssid[ilen] = 0; }
        else if (id == IE_DSPARAM && ilen >= 1) bss->channel = d[0];
        off += 2u + ilen;
    }
    return 0;
}

int ieee80211_build_probe_req(uint8_t *out, const uint8_t src[6], const char *ssid) {
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    struct ieee80211_hdr *h = (struct ieee80211_hdr *)out;
    memset(h, 0, sizeof(*h));
    h->frame_control = FC_TYPE_MGMT | MGMT_PROBE_REQ;
    memcpy(h->addr1, bcast, 6);    /* DA    */
    memcpy(h->addr2, src,   6);    /* SA    */
    memcpy(h->addr3, bcast, 6);    /* BSSID */
    uint32_t off = sizeof(*h);

    /* SSID IE (empty len = wildcard probe). */
    uint8_t slen = 0;
    if (ssid) while (ssid[slen] && slen < 32) slen++;
    out[off++] = IE_SSID; out[off++] = slen;
    for (uint8_t i = 0; i < slen; i++) out[off++] = (uint8_t)ssid[i];

    /* Supported-rates IE: 1/2/5.5/11 Mbps, high bit = basic rate. */
    static const uint8_t rates[] = { 0x82, 0x84, 0x8B, 0x96 };
    out[off++] = IE_RATES; out[off++] = (uint8_t)sizeof(rates);
    for (uint32_t i = 0; i < sizeof(rates); i++) out[off++] = rates[i];

    return (int)off;
}

const char *wifi_state_name(enum wifi_state s) {
    switch (s) {
    case WIFI_IDLE:     return "idle";
    case WIFI_SCANNING: return "scanning";
    case WIFI_AUTH:     return "authenticating";
    case WIFI_ASSOC:    return "associating";
    case WIFI_RUN:      return "running";
    default:            return "?";
    }
}
