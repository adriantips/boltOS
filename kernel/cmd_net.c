#include <stdint.h>
#include "commands.h"
#include "hw.h"
#include "net.h"
#include "netif.h"
#include "http.h"
#include "html.h"
#include "dom.h"
#include "layout.h"
#include "wifi.h"
#include "fs.h"
#include "kheap.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"
#include "commands.h"
#include "keyboard.h"
#include "firewall.h"

/* ===========================================================================
 *  Networking commands. ping/netinfo/download/browse/share ride the real IPv4
 *  stack over the e1000 driver. `share` does real LAN file transfer over UDP
 *  (discovery + stop-and-wait). The in-RAM firewall ruleset is real editable
 *  state. upload (HTTP POST) still reports honestly.
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
    if      (code == -3) kprintf("DNS lookup failed\n");
    else if (code == -4) kprintf("connection failed / timed out\n");
    else if (code == -5) kprintf("TLS handshake failed\n");
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

    int http = (strncmp(url, "http://", 7) == 0) || (strncmp(url, "https://", 8) == 0);
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
        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
            full[0] = 0; strcat(full, "http://"); strcat(full, url); url = full;
        }
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

/* domq URL|FILE SELECTOR -- parse a page into the BoltDOM tree and run a CSS
 * selector against it, printing each match's tag, id/class and text. Exercises
 * the real DOM tree + selector engine (querySelectorAll) deterministically. */
int cmd_domq(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: domq URL|FILE SELECTOR\n"); return 1; }
    const char *url = argv[1];
    char selbuf[160]; selbuf[0] = 0;            /* join argv[2..] so "a > b" survives word-split */
    for (int k = 2; k < argc; k++) { if (k > 2) strcat(selbuf, " "); strcat(selbuf, argv[k]); }
    const char *sel = selbuf;
    char *buf = 0; const char *data = 0; uint32_t dlen = 0;
    int http = (strncmp(url,"http://",7)==0) || (strncmp(url,"https://",8)==0);
    if (!http && url[0] != '/' && strncmp(url,"file:",5) != 0) {
        const char *slash = strchr(url,'/'), *dot = strchr(url,'.');
        if (dot && (!slash || dot < slash)) http = 1;
    }
    if (http) {
        if (!netif_default()) { kprintf("domq: no network\n"); return 1; }
        uint32_t cap = 256*1024; buf = (char *)kmalloc(cap);
        if (!buf) { kprintf("domq: out of memory\n"); return 1; }
        char full[300];
        if (strncmp(url,"http://",7)!=0 && strncmp(url,"https://",8)!=0) { full[0]=0; strcat(full,"http://"); strcat(full,url); url=full; }
        int code=0; char loc[256];
        int blen = http_get(url, buf, cap, &code, loc, sizeof(loc));
        if (blen < 0) { http_err(code); kfree(buf); return 1; }
        kprintf("HTTP %d, %d bytes\n", code, blen);
        data = buf; dlen = (uint32_t)blen;
    } else {
        const char *p = url; if (strncmp(p,"file:",5)==0) p += 5;
        fs_node *n = fs_lookup(p);
        if (!n || n->is_dir) { kprintf("domq: not found: %s\n", p); return 1; }
        data = (const char *)n->data; dlen = n->size;
    }
    dom_document *doc = dom_parse(data, dlen);
    if (!doc) { kprintf("domq: parse failed\n"); if (buf) kfree(buf); return 1; }
    dom_node *out[64];
    int n = dom_query_all(doc->root, sel, out, 64);
    kprintf("%d match(es) for '%s':\n", n, sel);
    char txt[160];
    for (int i = 0; i < n; i++) {
        const char *id = dom_attr_get(out[i], "id");
        const char *cl = dom_attr_get(out[i], "class");
        dom_inner_text(out[i], txt, sizeof(txt));
        kprintf("  <%s%s%s%s%s> %s\n", out[i]->tag,
                id ? " #" : "", id ? id : "",
                cl ? " ." : "", cl ? cl : "", txt);
    }
    dom_free(doc);
    if (buf) kfree(buf);
    return 0;
}

/* recursively print the layout box tree with computed geometry + key styles */
static void print_box(layout_box *b, int depth) {
    if (!b) return;
    char ind[24]; int n = depth*2 < 22 ? depth*2 : 22; for (int i=0;i<n;i++) ind[i]=' '; ind[n]=0;
    if (b->is_text) {
        char t[40]; int k=0; for (const char*p=b->text; *p && k<38; p++) t[k++]= (*p=='\n'?' ':*p); t[k]=0;
        kprintf("%s#text [x%d y%d w%d h%d] \"%s\"\n", ind, b->x, b->y, b->w, b->h, t);
    } else {
        const char *tag = b->node ? b->node->tag : "?";
        static const char *dn[] = {"inline","block","inline-block","list-item","flex","grid","none"};
        kprintf("%s<%s> %s [x%d y%d w%d h%d] m%d,%d,%d,%d p%d fs%d col=%x bg=%x\n",
                ind, tag, dn[b->st.display], b->x, b->y, b->w, b->h,
                b->st.margin[0],b->st.margin[1],b->st.margin[2],b->st.margin[3],
                b->st.padding[0], b->st.fscale, b->st.color & 0xFFFFFF, b->st.bg & 0xFFFFFF);
    }
    for (layout_box *c=b->first_child; c; c=c->next) print_box(c, depth+1);
}

/* domlayout URL|FILE [css] -- parse to DOM, run the cascade + box layout engine
 * at 760px viewport, and print the computed box tree. */
int cmd_domlayout(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: domlayout URL|FILE\n"); return 1; }
    const char *url = argv[1];
    char *buf = 0; const char *data = 0; uint32_t dlen = 0;
    int http = (strncmp(url,"http://",7)==0)||(strncmp(url,"https://",8)==0);
    if (!http && url[0]!='/' && strncmp(url,"file:",5)!=0) { const char *sl=strchr(url,'/'),*dt=strchr(url,'.'); if(dt&&(!sl||dt<sl))http=1; }
    if (http) {
        if (!netif_default()) { kprintf("domlayout: no network\n"); return 1; }
        uint32_t cap=256*1024; buf=(char*)kmalloc(cap); if(!buf){kprintf("oom\n");return 1;}
        char full[300]; if(strncmp(url,"http://",7)&&strncmp(url,"https://",8)){full[0]=0;strcat(full,"http://");strcat(full,url);url=full;}
        int code=0; char loc[256]; int blen=http_get(url,buf,cap,&code,loc,sizeof loc);
        if(blen<0){http_err(code);kfree(buf);return 1;} kprintf("HTTP %d, %d bytes\n",code,blen);
        data=buf; dlen=(uint32_t)blen;
    } else {
        const char *p=url; if(strncmp(p,"file:",5)==0)p+=5; fs_node*nf=fs_lookup(p);
        if(!nf||nf->is_dir){kprintf("domlayout: not found: %s\n",p);return 1;} data=(const char*)nf->data; dlen=nf->size;
    }
    /* pull inline <style> blocks out for the cascade (external <link> skipped here) */
    char *css=(char*)kmalloc(64*1024); uint32_t clen=0;
    if(css){ for(uint32_t i=0;i+7<dlen;i++){ if((data[i]=='<')&&(data[i+1]=='s'||data[i+1]=='S')){
            if(strncmp(data+i,"<style",6)==0||strncmp(data+i,"<STYLE",6)==0){ uint32_t j=i+6; while(j<dlen&&data[j]!='>')j++; j++;
                uint32_t s0=j; while(j+7<dlen&&!(data[j]=='<'&&(data[j+1]=='/')&&(data[j+2]=='s'||data[j+2]=='S')))j++;
                uint32_t n=j-s0; if(clen+n<64*1024-1){memcpy(css+clen,data+s0,n);clen+=n;css[clen++]='\n';} i=j; } } } css[clen]=0; }
    dom_document *doc=dom_parse(data,dlen);
    if(!doc){kprintf("domlayout: dom parse failed\n");if(css)kfree(css);if(buf)kfree(buf);return 1;}
    layout_tree *lt=layout_build(doc,css,clen,760);
    if(!lt){kprintf("domlayout: layout failed\n");dom_free(doc);if(css)kfree(css);if(buf)kfree(buf);return 1;}
    kprintf("viewport 760px, doc height %d px:\n", lt->doc_h);
    print_box(lt->root, 0);
    layout_free(lt); dom_free(doc); if(css)kfree(css); if(buf)kfree(buf);
    return 0;
}

int cmd_upload(int argc, char **argv) {
    (void)argv;
    if (argc < 2) { kprintf("usage: upload FILE\n"); return 1; }
    no_net("upload");
    return 1;
}

/* ===========================================================================
 *  share -- real LAN file transfer over UDP (port 7373).
 *
 *  One machine runs `share recv` and listens; another runs `share` (which
 *  broadcast-discovers receivers on the same Wi-Fi/LAN, lists them, and lets
 *  you pick one) or `share send IP PATH` to target directly. Transfer is a
 *  simple stop-and-wait reliable protocol over UDP: OFFER/ACCEPT to set up,
 *  DATA/ACK per 1 KB chunk with retransmit, then DONE.
 * ===========================================================================*/
#define SHARE_PORT   7373
#define SHARE_MAGIC  0x42534852u            /* 'BSHR' */
#define SHARE_CHUNK  1024
#define SHARE_MAXFILE (4u * 1024 * 1024)    /* 4 MiB cap (kernel heap budget) */
enum { SH_DISC = 1, SH_ANN, SH_OFFER, SH_ACCEPT, SH_DATA, SH_ACK, SH_DONE };

struct shdr {
    uint32_t magic;
    uint8_t  type, pad;
    uint16_t seq;
    uint32_t arg;
} __attribute__((packed));

static struct {
    int      active;                /* listener registered                     */
    int      mode;                  /* 0 = sender/discovery, 1 = receiver       */
    char     myname[32];            /* advertised name (receiver)              */
    /* discovery results (sender) */
    struct { uint32_t ip; char name[32]; } peers[16];
    int      npeer;
    /* sender transfer state (written by the RX callback) */
    volatile int      got_accept;
    volatile int      last_ack;     /* highest DATA seq the peer ACKed, or -1   */
    /* receiver transfer state */
    volatile int      recv_busy, recv_done;
    uint32_t recv_src;
    char     recv_name[64];
    uint32_t recv_size, recv_got;
    uint16_t recv_expect;
    uint8_t *recv_buf;
} S;

/* build + send one share packet (payload optional, capped to one chunk) */
static void sh_send(uint32_t ip, uint8_t type, uint16_t seq, uint32_t arg,
                    const void *pl, uint16_t pll) {
    struct netif *nif = netif_default();
    if (!nif) return;
    uint8_t buf[sizeof(struct shdr) + SHARE_CHUNK];
    struct shdr *h = (struct shdr *)buf;
    h->magic = htonl(SHARE_MAGIC);
    h->type  = type; h->pad = 0;
    h->seq   = htons(seq);
    h->arg   = htonl(arg);
    if (pll > SHARE_CHUNK) pll = SHARE_CHUNK;
    if (pl && pll) memcpy(buf + sizeof(*h), pl, pll);
    udp_send(nif, ip, SHARE_PORT, SHARE_PORT, buf, (uint16_t)(sizeof(*h) + pll));
}

/* the single UDP listener: dispatches by our current role */
static void share_cb(uint32_t src, uint16_t sport, const uint8_t *data, uint16_t len) {
    (void)sport;
    if (len < sizeof(struct shdr)) return;
    const struct shdr *h = (const struct shdr *)data;
    if (ntohl(h->magic) != SHARE_MAGIC) return;
    const uint8_t *pl = data + sizeof(*h);
    uint16_t pll = (uint16_t)(len - sizeof(*h));
    uint8_t  type = h->type;

    if (S.mode == 1) {                          /* ---- receiver role ---- */
        switch (type) {
        case SH_DISC:                           /* advertise ourselves */
            sh_send(src, SH_ANN, 0, 0, S.myname, (uint16_t)strlen(S.myname));
            break;
        case SH_OFFER: {                        /* incoming file */
            if (S.recv_busy && src != S.recv_src) break;   /* one at a time */
            uint32_t size = ntohl(h->arg);
            if (size > SHARE_MAXFILE) break;
            if (!S.recv_busy) {
                uint32_t i = 0;
                for (; i < pll && i < sizeof(S.recv_name) - 1; i++) S.recv_name[i] = (char)pl[i];
                S.recv_name[i] = 0;
                S.recv_buf = (uint8_t *)kmalloc(size ? size : 1);
                if (!S.recv_buf) break;
                S.recv_size = size; S.recv_got = 0; S.recv_expect = 0;
                S.recv_src = src; S.recv_busy = 1;
            }
            sh_send(src, SH_ACCEPT, 0, 0, 0, 0);           /* (re)accept */
            break; }
        case SH_DATA: {
            if (!S.recv_busy || src != S.recv_src) break;
            uint16_t seq = ntohs(h->seq);
            if (seq == S.recv_expect) {
                uint32_t room = S.recv_size - S.recv_got;
                uint32_t take = pll < room ? pll : room;
                if (take) memcpy(S.recv_buf + S.recv_got, pl, take);
                S.recv_got += take; S.recv_expect++;
            }
            sh_send(src, SH_ACK, seq, 0, 0, 0);            /* ack (even dups) */
            break; }
        case SH_DONE:
            if (!S.recv_busy || src != S.recv_src) break;
            sh_send(src, SH_ACK, h->seq ? ntohs(h->seq) : 0, 1, 0, 0);
            S.recv_done = 1;
            break;
        }
    } else {                                    /* ---- sender / discovery role ---- */
        switch (type) {
        case SH_ANN: {                          /* a receiver answered discovery */
            for (int i = 0; i < S.npeer; i++) if (S.peers[i].ip == src) return;
            if (S.npeer < 16) {
                S.peers[S.npeer].ip = src;
                uint32_t i = 0;
                for (; i < pll && i < sizeof(S.peers[0].name) - 1; i++)
                    S.peers[S.npeer].name[i] = (char)pl[i];
                S.peers[S.npeer].name[i] = 0;
                S.npeer++;
            }
            break; }
        case SH_ACCEPT: S.got_accept = 1; break;
        case SH_ACK:    S.last_ack = ntohs(h->seq); break;
        }
    }
}

/* pump the stack until *flag reaches want, or timeout. returns 1 if reached. */
static int sh_wait(volatile int *flag, int want, uint32_t ms) {
    uint64_t start = pit_ticks();
    while (pit_ticks() - start < ms) {
        netif_poll_all();
        if (*flag == want) return 1;
        __asm__ volatile("hlt");
    }
    return *flag == want;
}

/* send file f to a known peer. assumes the listener is registered, mode 0. */
static int share_send_to(uint32_t ip, fs_node *f) {
    const char *base = strrchr(f->name, '/');
    base = base ? base + 1 : f->name;
    uint32_t size = f->size;

    /* handshake: OFFER -> ACCEPT, retransmit a few times */
    S.got_accept = 0;
    int ok = 0;
    for (int tries = 0; tries < 12 && !ok; tries++) {
        sh_send(ip, SH_OFFER, 0, size, base, (uint16_t)strlen(base));
        ok = sh_wait(&S.got_accept, 1, 300);
    }
    if (!ok) { kprintf("share: target did not accept (no response)\n"); return 1; }

    /* DATA chunks, stop-and-wait with retransmit */
    uint32_t nchunks = size ? (size + SHARE_CHUNK - 1) / SHARE_CHUNK : 0;
    kprintf("sending '%s' (%u bytes, %u chunks)\n", base, size, nchunks);
    for (uint32_t seq = 0; seq < nchunks; seq++) {
        uint32_t off = seq * SHARE_CHUNK;
        uint16_t clen = (uint16_t)((size - off < SHARE_CHUNK) ? size - off : SHARE_CHUNK);
        S.last_ack = -1;
        int acked = 0;
        for (int tries = 0; tries < 20 && !acked; tries++) {
            sh_send(ip, SH_DATA, (uint16_t)seq, 0, f->data + off, clen);
            uint64_t start = pit_ticks();
            while (pit_ticks() - start < 250) {
                netif_poll_all();
                if (S.last_ack == (int)seq) { acked = 1; break; }
                __asm__ volatile("hlt");
            }
        }
        if (!acked) { kprintf("\nshare: transfer stalled at chunk %u\n", seq); return 1; }
        if ((seq & 31) == 0 || seq == nchunks - 1) { kputc('.'); }
    }
    /* DONE (best effort, a few times) */
    for (int i = 0; i < 4; i++) { sh_send(ip, SH_DONE, 0, nchunks, 0, 0); sh_wait(&S.got_accept, 2, 60); }
    kprintf("\nsent %u bytes to ", size); print_ip(ip); kprintf("\n");
    return 0;
}

static void share_name_default(char *out, int cap) {
    /* "bolt-<last octet of our IP>" */
    char num[8]; sh_utoa(net_ip & 0xFF, num);
    int n = 0; const char *p = "bolt-";
    while (*p && n < cap - 1) out[n++] = *p++;
    for (const char *q = num; *q && n < cap - 1; ) out[n++] = *q++;
    out[n] = 0;
}

/* `share recv [name]` -- listen, advertise, receive one file, save it. */
static int share_recv(int argc, char **argv) {
    if (!netif_default()) { kprintf("share: no network interface\n"); return 1; }
    memset(&S, 0, sizeof(S));
    S.mode = 1;
    if (argc >= 3) { uint32_t n = 0; for (; argv[2][n] && n < sizeof(S.myname) - 1; n++) S.myname[n] = argv[2][n]; S.myname[n] = 0; }
    else share_name_default(S.myname, sizeof(S.myname));

    udp_listen(SHARE_PORT, share_cb); S.active = 1;
    kprintf("share: listening as '%s' on ", S.myname); print_ip(net_ip);
    kprintf(":%d\nwaiting for a file... (press ESC to cancel)\n", SHARE_PORT);

    int cancelled = 0;
    while (!S.recv_done) {
        netif_poll_all();
        int k = kbd_trygetc();
        if (k == 27) { cancelled = 1; break; }       /* ESC */
        __asm__ volatile("hlt");
    }

    int rc = 0;
    if (S.recv_done) {
        fs_node *fn = fs_lookup(S.recv_name);
        if (!fn) fn = fs_create(S.recv_name, 0);
        if (!fn || fn->is_dir) { kprintf("share: cannot save '%s'\n", S.recv_name); rc = 1; }
        else {
            fs_write(fn, S.recv_buf, S.recv_got);
            kprintf("received '%s' (%u bytes) from ", S.recv_name, S.recv_got);
            print_ip(S.recv_src); kprintf(" -> saved\n");
        }
    } else if (cancelled) {
        kprintf("share: cancelled\n");
    }
    udp_unlisten(SHARE_PORT);
    if (S.recv_buf) kfree(S.recv_buf);
    S.active = 0;
    return rc;
}

int cmd_share(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage:\n");
        kprintf("  share recv [name]     listen and receive a file (run on the target)\n");
        kprintf("  share PATH            discover targets on the LAN, pick one, send PATH\n");
        kprintf("  share send IP PATH    send PATH directly to a known IP\n");
        return 1;
    }
    if (strcmp(argv[1], "recv") == 0) return share_recv(argc, argv);
    if (!netif_default()) { kprintf("share: no network interface\n"); return 1; }

    /* `share send IP PATH` -- direct */
    if (strcmp(argv[1], "send") == 0) {
        if (argc < 4) { kprintf("usage: share send IP PATH\n"); return 1; }
        int ok; uint32_t ip = net_parse_ipv4(argv[2], &ok);
        if (!ok) { kprintf("share: invalid address '%s'\n", argv[2]); return 1; }
        fs_node *f = fs_lookup(argv[3]);
        if (!f || f->is_dir || !f->data) { kprintf("share: not a file: %s\n", argv[3]); return 1; }
        if (f->size > SHARE_MAXFILE) { kprintf("share: file too large (max 4 MiB)\n"); return 1; }
        memset(&S, 0, sizeof(S)); S.mode = 0;
        udp_listen(SHARE_PORT, share_cb); S.active = 1;
        int rc = share_send_to(ip, f);
        udp_unlisten(SHARE_PORT); S.active = 0;
        return rc;
    }

    /* `share PATH` -- discover targets, choose one, send */
    fs_node *f = fs_lookup(argv[1]);
    if (!f || f->is_dir || !f->data) { kprintf("share: not a file: %s\n", argv[1]); return 1; }
    if (f->size > SHARE_MAXFILE) { kprintf("share: file too large (max 4 MiB)\n"); return 1; }

    memset(&S, 0, sizeof(S)); S.mode = 0;
    udp_listen(SHARE_PORT, share_cb); S.active = 1;

    kprintf("share: searching for targets on the LAN...\n");
    uint64_t start = pit_ticks();
    int blasts = 0;
    while (pit_ticks() - start < 1600) {
        if ((pit_ticks() - start) >= (uint64_t)blasts * 400) {   /* DISC every 400ms */
            sh_send(0xFFFFFFFFu, SH_DISC, 0, 0, 0, 0); blasts++;
        }
        netif_poll_all();
        __asm__ volatile("hlt");
    }

    if (S.npeer == 0) {
        kprintf("share: no targets found. (run 'share recv' on the other machine)\n");
        udp_unlisten(SHARE_PORT); S.active = 0;
        return 1;
    }
    kprintf("found %d target(s):\n", S.npeer);
    for (int i = 0; i < S.npeer; i++) {
        kprintf("  %d) %s  ", i + 1, S.peers[i].name); print_ip(S.peers[i].ip); kprintf("\n");
    }
    kprintf("select target [1-%d, 0=cancel]: ", S.npeer);
    char sel[16]; sh_readline(sel, sizeof(sel));
    int idx = atoi(sel);
    int rc = 1;
    if (idx >= 1 && idx <= S.npeer) rc = share_send_to(S.peers[idx - 1].ip, f);
    else kprintf("share: cancelled\n");

    udp_unlisten(SHARE_PORT); S.active = 0;
    return rc;
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

/* firewall: a real packet filter. The ruleset lives in net/firewall.c and the
 * IPv4 layer drops anything a BLOCK rule rejects, inbound and outbound. */
static void fw_usage(void) {
    kprintf("usage: firewall [on|off|status|stats|clear|default allow|block]\n");
    kprintf("       firewall add <allow|block> [in|out] [tcp|udp|icmp]\n");
    kprintf("                    [ip ADDR[/PREFIX]] [port N]\n");
    kprintf("  e.g. firewall add block out tcp port 80\n");
    kprintf("       firewall add block in icmp\n");
    kprintf("       firewall add block ip 93.184.216.0/24\n");
}

int cmd_firewall(int argc, char **argv) {
    const char *act = (argc > 1) ? argv[1] : "status";
    if (strcmp(act, "on") == 0)        { fw_set_enabled(1); kprintf("firewall enabled\n"); }
    else if (strcmp(act, "off") == 0)  { fw_set_enabled(0); kprintf("firewall disabled\n"); }
    else if (strcmp(act, "clear") == 0){ fw_clear();        kprintf("firewall rules cleared\n"); }
    else if (strcmp(act, "default") == 0) {
        if (argc < 3) { fw_usage(); return 1; }
        if (strcmp(argv[2], "allow") == 0)      fw_set_default(1);
        else if (strcmp(argv[2], "block") == 0) fw_set_default(0);
        else { fw_usage(); return 1; }
        kprintf("default policy: %s\n", fw_get_default() ? "allow" : "block");
    }
    else if (strcmp(act, "add") == 0) {
        if (argc < 3) { fw_usage(); return 1; }
        /* rebuild the raw rule text for display */
        char raw[56]; uint32_t L = 0;
        for (int i = 2; i < argc && L < sizeof(raw) - 1; i++) {
            if (i > 2 && L < sizeof(raw) - 1) raw[L++] = ' ';
            for (const char *s = argv[i]; *s && L < sizeof(raw) - 1; ) raw[L++] = *s++;
        }
        raw[L] = 0;
        int rc = fw_add(argc - 2, argv + 2, raw);
        if (rc == 0)       kprintf("rule added (#%d)\n", fw_count());
        else if (rc == -2) kprintf("firewall: rule table full\n");
        else { kprintf("firewall: bad rule\n"); fw_usage(); return 1; }
    }
    else if (strcmp(act, "stats") == 0) {
        uint32_t p, b; fw_stats(&p, &b);
        kprintf("firewall: %s  passed=%u blocked=%u\n",
                fw_enabled() ? "ENABLED" : "disabled", p, b);
    }
    else if (strcmp(act, "list") == 0 || strcmp(act, "status") == 0) {
        kprintf("firewall: %s, default %s, %d rule(s)\n",
                fw_enabled() ? "ENABLED" : "disabled",
                fw_get_default() ? "allow" : "block", fw_count());
        for (int i = 0; i < fw_count(); i++)
            kprintf("  %d) %s\n", i + 1, fw_rule_text(i));
    }
    else { fw_usage(); }
    return 0;
}
