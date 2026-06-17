#include <stdint.h>
#include "commands.h"
#include "hw.h"
#include "net.h"
#include "netif.h"
#include "http.h"
#include "html.h"
#include "wifi.h"
#include "fs.h"
#include "kheap.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  Networking commands. ping/netinfo now ride the real IPv4 stack (P3) over the
 *  e1000 driver. Transports without a stack yet (download/upload/share) still
 *  report honestly. The in-RAM firewall ruleset is real editable state.
 * ===========================================================================*/

static int find_nic(struct pci_dev *out) {
    struct pci_dev pds[32];
    int n = pci_scan(pds, 32);
    for (int i = 0; i < n; i++)
        if (pds[i].class == 0x02 || pds[i].class == 0x0D) { *out = pds[i]; return 1; }
    return 0;
}
static void no_net(const char *cmd) {
    kprintf("%s: not implemented (no transport layer yet)\n", cmd);
}
static void print_ip(uint32_t ip) {
    kprintf("%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
            (ip >> 8) & 0xFF, ip & 0xFF);
}
static void print_mac(const uint8_t *m) {   /* kprintf has no %02x */
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        char s[3] = { h[m[i] >> 4], h[m[i] & 0xF], 0 };
        kprintf("%s%s", s, i < 5 ? ":" : "");
    }
}

int cmd_netinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    struct netif *nif = netif_default();
    if (!nif) {
        struct pci_dev nic;
        kprintf("interfaces: none up\n");
        if (find_nic(&nic))
            kprintf("hardware  : PCI %x:%x.%x %x:%x (%s) - no driver bound\n",
                    nic.bus, nic.slot, nic.func, nic.vendor, nic.device,
                    pci_class_name(nic.class));
        else
            kprintf("hardware  : no PCI network controller present\n");
        return 0;
    }
    kprintf("%s  link %s\n", nif->name, nif->link_up ? "up" : "down");
    kprintf("  mac    : "); print_mac(nif->mac); kprintf("\n");
    kprintf("  ip     : "); print_ip(net_ip);   kprintf("\n");
    kprintf("  mask   : "); print_ip(net_mask); kprintf("\n");
    kprintf("  gateway: "); print_ip(net_gw);   kprintf("\n");
    kprintf("  rx     : %lu pkts / %lu bytes\n", nif->rx_packets, nif->rx_bytes);
    kprintf("  tx     : %lu pkts / %lu bytes\n", nif->tx_packets, nif->tx_bytes);
    return 0;
}

int cmd_ping(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: ping A.B.C.D\n"); return 1; }
    struct netif *nif = netif_default();
    if (!nif) { kprintf("ping: no network interface\n"); return 1; }

    int ok;
    uint32_t dst = net_parse_ipv4(argv[1], &ok);
    if (!ok) { kprintf("ping: invalid address '%s' (use a.b.c.d)\n", argv[1]); return 1; }

    uint32_t nexthop = ((dst & net_mask) == (net_ip & net_mask)) ? dst : net_gw;
    uint8_t mac[6];
    if (!arp_resolve(nif, nexthop, mac, 1000)) {
        kprintf("ping: ARP timeout resolving "); print_ip(nexthop); kprintf("\n");
        return 1;
    }

    uint16_t id = 0x4254;          /* 'BT' */
    int sent = 0, recv = 0;
    kprintf("PING "); print_ip(dst); kprintf(" 32 bytes of data:\n");
    for (uint16_t seq = 1; seq <= 4; seq++) {
        uint64_t t0 = pit_ticks();
        if (icmp_echo_send(nif, dst, id, seq) < 0) { kprintf("  send failed seq=%u\n", seq); continue; }
        sent++;
        if (icmp_wait_reply(id, seq, 1000)) {
            recv++;
            kprintf("  reply from "); print_ip(dst);
            kprintf(": seq=%u time=%lums\n", seq, pit_ticks() - t0);
        } else {
            kprintf("  request timed out seq=%u\n", seq);
        }
    }
    kprintf("--- "); print_ip(dst);
    kprintf(" statistics: %d sent, %d received, %d%% loss\n",
            sent, recv, sent ? (100 * (sent - recv) / sent) : 0);
    return recv ? 0 : 1;
}

int cmd_trace(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: trace HOST\n"); return 1; }
    kprintf("trace to %s: ", argv[1]);
    no_net("trace");
    return 1;
}

/* print an HTTP error from a negative/zero status code */
static void http_err(int code) {
    if      (code == -2) kprintf("https is not supported (http only)\n");
    else if (code == -3) kprintf("DNS lookup failed\n");
    else if (code == -4) kprintf("connection failed / timed out\n");
    else                 kprintf("request failed\n");
}

int cmd_download(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: download URL [dest]\n"); return 1; }
    if (!netif_default()) { kprintf("download: no network interface\n"); return 1; }

    uint32_t cap = 256 * 1024;
    char *buf = (char *)kmalloc(cap);
    if (!buf) { kprintf("download: out of memory\n"); return 1; }

    int code = 0; char loc[256];
    int blen = http_get(argv[1], buf, cap, &code, loc, sizeof(loc));
    if (blen < 0) { http_err(code); kfree(buf); return 1; }

    /* destination name: explicit arg, else the URL's last path component */
    const char *dest = (argc >= 3) ? argv[2] : 0;
    char name[64];
    if (!dest) {
        const char *base = strrchr(argv[1], '/');
        base = (base && base[1]) ? base + 1 : "index.html";
        uint32_t i = 0; while (base[i] && i < sizeof(name) - 1) { name[i] = base[i]; i++; } name[i] = 0;
        dest = name;
    }
    fs_node *f = fs_lookup(dest);
    if (!f) f = fs_create(dest, 0);
    if (!f || f->is_dir) { kprintf("download: cannot write '%s'\n", dest); kfree(buf); return 1; }
    fs_write(f, buf, (uint32_t)blen);
    kprintf("downloaded %d bytes (HTTP %d) -> %s\n", blen, code, dest);
    kfree(buf);
    return 0;
}

/* render the text of a page/file to the terminal (no layout, just flow) */
static void browse_print(html_doc *d) {
    if (!d) { kprintf("(empty)\n"); return; }
    if (d->title) kprintf("== %s ==\n", d->title);
    for (int i = 0; i < d->nruns; i++) {
        html_run *r = &d->runs[i];
        if (r->brk) { kprintf("\n"); if (r->brk >= 2) kprintf("\n"); }
        kprintf("%s ", r->text);
        if (r->link >= 0 && r->link < d->nhrefs) kprintf("<%s> ", d->hrefs[r->link]);
    }
    kprintf("\n");
}

int cmd_browse(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: browse URL|FILE\n"); return 1; }
    const char *url = argv[1];
    html_doc *d = 0;

    int http = (strncmp(url, "http://", 7) == 0);
    if (!http && url[0] != '/' && strncmp(url, "file:", 5) != 0) {
        const char *slash = strchr(url, '/'), *dot = strchr(url, '.');
        if (dot && (!slash || dot < slash)) http = 1;     /* looks like a host */
    }

    if (http) {
        if (!netif_default()) { kprintf("browse: no network interface\n"); return 1; }
        uint32_t cap = 256 * 1024;
        char *buf = (char *)kmalloc(cap);
        if (!buf) { kprintf("browse: out of memory\n"); return 1; }
        char full[300];
        if (strncmp(url, "http://", 7) != 0) { full[0] = 0; strcat(full, "http://"); strcat(full, url); url = full; }
        int code = 0; char loc[256];
        int blen = http_get(url, buf, cap, &code, loc, sizeof(loc));
        if (blen < 0) { http_err(code); kfree(buf); return 1; }
        kprintf("HTTP %d, %d bytes\n", code, blen);
        d = html_parse(buf, (uint32_t)blen);
        browse_print(d);
        html_free(d);
        kfree(buf);
        return 0;
    }

    const char *p = url; if (strncmp(p, "file:", 5) == 0) p += 5;
    fs_node *n = fs_lookup(p);
    if (!n) { kprintf("browse: not found: %s\n", p); return 1; }
    if (n->is_dir) { kprintf("browse: '%s' is a directory\n", p); return 1; }
    int ishtml = 0; uint32_t ln = strlen(n->name);
    if (ln >= 5 && strcmp(n->name + ln - 5, ".html") == 0) ishtml = 1;
    if (ln >= 4 && strcmp(n->name + ln - 4, ".htm") == 0)  ishtml = 1;
    d = ishtml ? html_parse((const char *)n->data, n->size)
               : html_parse_text((const char *)n->data, n->size);
    browse_print(d);
    html_free(d);
    return 0;
}

int cmd_upload(int argc, char **argv) {
    (void)argv;
    if (argc < 2) { kprintf("usage: upload FILE\n"); return 1; }
    no_net("upload");
    return 1;
}

int cmd_share(int argc, char **argv) {
    (void)argv;
    if (argc < 2) { kprintf("usage: share PATH\n"); return 1; }
    no_net("share");
    return 1;
}

int cmd_wifi(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Wi-Fi (802.11) softMAC: %s\n", wifi_state_name(WIFI_IDLE));

    struct pci_dev nic;
    int have_wl = find_nic(&nic) && nic.class == 0x0D;
    if (have_wl)
        kprintf("  radio   : controller %x:%x present, no driver bound\n", nic.vendor, nic.device);
    else
        kprintf("  radio   : none (ath9k-htc needs a USB host stack -- not built)\n");
    kprintf("  scan    : unavailable without a radio driver\n");

    /* the MAC-layer frame helpers are real and used by the stack; the live data
     * path today is the wired NIC, which the browser and net commands ride. */
    struct netif *nif = netif_default();
    if (nif)
        kprintf("  uplink  : %s (%s) carries all traffic for now\n",
                nif->name, nif->link_up ? "up" : "down");
    else
        kprintf("  uplink  : no interface up\n");
    return 0;
}

/* ports: no TCP/UDP listeners (no stack). Show the real hardware I/O port map
 * instead, which is the closest thing BoltOS actually has. */
int cmd_ports(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("network ports: none (no TCP/UDP stack)\n");
    kprintf("hardware I/O port map:\n");
    kprintf("  0x0020-0x0021  PIC master\n");
    kprintf("  0x0040-0x0043  PIT timer\n");
    kprintf("  0x0060,0x0064  PS/2 keyboard\n");
    kprintf("  0x0070-0x0071  CMOS/RTC\n");
    kprintf("  0x00A0-0x00A1  PIC slave\n");
    kprintf("  0x03F8-0x03FF  COM1 serial\n");
    kprintf("  0x0CF8-0x0CFF  PCI config\n");
    return 0;
}

/* scan: discover devices. No network, so this enumerates the PCI bus - the
 * real devices attached to the machine. */
int cmd_scan(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("no network to scan; enumerating PCI bus instead:\n");
    struct pci_dev pds[32];
    int n = pci_scan(pds, 32);
    for (int i = 0; i < n; i++) {
        kprintf("  %x:%x.%x  vendor %x  device %x  ",
                pds[i].bus, pds[i].slot, pds[i].func, pds[i].vendor, pds[i].device);
        sh_pad(pci_class_name(pds[i].class), 14);
        kprintf("\n");
    }
    kprintf("%d device(s)\n", n);
    return 0;
}

/* firewall: an in-RAM ruleset. It is real state you can edit/list, but with no
 * network stack it filters nothing - kept for when a NIC driver lands. */
#define FW_MAX 16
#define FW_LEN 48
static char fw_rules[FW_MAX][FW_LEN];
static int  fw_count;
static int  fw_enabled;

int cmd_firewall(int argc, char **argv) {
    const char *act = (argc > 1) ? argv[1] : "status";
    if (strcmp(act, "on") == 0)        { fw_enabled = 1; kprintf("firewall enabled\n"); }
    else if (strcmp(act, "off") == 0)  { fw_enabled = 0; kprintf("firewall disabled\n"); }
    else if (strcmp(act, "clear") == 0){ fw_count = 0;   kprintf("firewall rules cleared\n"); }
    else if (strcmp(act, "add") == 0) {
        if (argc < 3) { kprintf("usage: firewall add RULE\n"); return 1; }
        if (fw_count >= FW_MAX) { kprintf("firewall: rule table full\n"); return 1; }
        char *r = fw_rules[fw_count];
        uint32_t L = 0;
        for (int i = 2; i < argc && L < FW_LEN - 1; i++) {
            if (i > 2 && L < FW_LEN - 1) r[L++] = ' ';
            for (const char *s = argv[i]; *s && L < FW_LEN - 1; ) r[L++] = *s++;
        }
        r[L] = 0;
        fw_count++;
        kprintf("rule added (#%d)\n", fw_count);
    } else if (strcmp(act, "list") == 0 || strcmp(act, "status") == 0) {
        kprintf("firewall: %s, %d rule(s)\n", fw_enabled ? "ENABLED" : "disabled", fw_count);
        for (int i = 0; i < fw_count; i++) kprintf("  %d) %s\n", i + 1, fw_rules[i]);
        kprintf("(advisory: no NIC bound, so nothing is actually filtered)\n");
    } else {
        kprintf("usage: firewall [on|off|add RULE|list|clear]\n");
    }
    return 0;
}
