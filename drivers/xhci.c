#include <stdint.h>
#include <stddef.h>
#include "hw.h"
#include "dma.h"
#include "mmio.h"
#include "string.h"
#include "kprintf.h"
#include "xhci.h"

/* ===========================================================================
 *  xHCI USB host controller driver (polled).
 *
 *  Brings up the controller (DCBAA, command ring, single-segment event ring,
 *  scratchpad buffers), then for every connected root-hub port: reset it,
 *  Enable Slot, Address Device (BSR=0), and walk EP0 control transfers to read
 *  the device + configuration descriptors. Devices are reported and remembered.
 *
 *  No interrupts: completions are found by polling the event-ring cycle bit.
 *  The controller's interrupts are masked (IMAN, PCI Interrupt-Disable) so an
 *  unhandled completion can't storm the IDT -- same lesson as the NVMe driver.
 *
 *  This is a faithful subset of the spec: enough to enumerate and classify
 *  HID/mass-storage/hub devices over EP0. Interrupt-IN endpoint polling for
 *  HID boot input is layered on top via xhci_get_devices().
 * ===========================================================================*/

/* --- capability registers (BAR0 + 0) -------------------------------------- */
#define CAP_CAPLENGTH   0x00   /* byte: operational regs offset    */
#define CAP_HCSPARAMS1  0x04
#define CAP_HCSPARAMS2  0x08
#define CAP_HCCPARAMS1  0x10
#define CAP_DBOFF       0x14
#define CAP_RTSOFF      0x18

/* --- operational registers (BAR0 + CAPLENGTH) ----------------------------- */
#define OP_USBCMD       0x00
#define OP_USBSTS       0x04
#define OP_PAGESIZE     0x08
#define OP_DNCTRL       0x14
#define OP_CRCR         0x18   /* 64-bit */
#define OP_DCBAAP       0x30   /* 64-bit */
#define OP_CONFIG       0x38
#define OP_PORTSC(p)    (0x400 + (p) * 0x10)   /* p is 0-based here */

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRST    (1u << 1)
#define USBCMD_INTE     (1u << 2)
#define USBSTS_HCH      (1u << 0)
#define USBSTS_CNR      (1u << 11)

#define PORTSC_CCS      (1u << 0)
#define PORTSC_PED      (1u << 1)
#define PORTSC_PR       (1u << 4)
#define PORTSC_PP       (1u << 9)
#define PORTSC_PRC      (1u << 21)
/* write-1-to-clear status change bits we must preserve-mask when writing */
#define PORTSC_RW1CS    (0x00FE0000u)   /* CSC..CEC change bits */

/* --- runtime / interrupter 0 (BAR0 + RTSOFF + 0x20) ----------------------- */
#define IR0_IMAN        0x00
#define IR0_IMOD        0x04
#define IR0_ERSTSZ      0x08
#define IR0_ERSTBA      0x10   /* 64-bit */
#define IR0_ERDP        0x18   /* 64-bit */

/* --- TRB types ------------------------------------------------------------ */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_TR_EVENT    32
#define TRB_CMD_EVENT   33
#define TRB_PORTSC_EVENT 34

#define CC_SUCCESS      1
#define CC_SHORT_PACKET 13

#define RING_SZ   16     /* TRBs per ring (incl. link)            */
#define POLL_MAX  5000000ull

struct trb { uint32_t d[4]; };

struct ring {
    volatile uint32_t *trb;   /* RING_SZ * 4 dwords */
    uint64_t phys;
    uint32_t enq;
    uint32_t cycle;
    struct dma_buf buf;
};

struct usbdev {
    int      used;
    uint8_t  slot, port, speed;
    uint16_t vendor, product;
    uint8_t  dev_class, if_class;
};

#define MAX_DEV 8

static struct {
    int present;
    volatile void *cap;        /* BAR0 base (capability regs)   */
    volatile void *op;         /* operational regs              */
    volatile void *rt;         /* runtime regs                  */
    volatile uint32_t *db;     /* doorbell array                */
    uint32_t maxslots, maxports;
    int      csz;              /* context size: 0=32B, 1=64B    */

    struct dma_buf dcbaa_buf;
    volatile uint64_t *dcbaa;

    struct ring cmd;
    struct ring evt;
    uint32_t evt_cycle;
    uint32_t evt_deq;

    struct usbdev dev[MAX_DEV];
    int ndev;
} xh;

static inline uint32_t cap_rd(uint32_t o)  { return mmio_read32(xh.cap, o); }
static inline uint32_t op_rd(uint32_t o)   { return mmio_read32(xh.op, o); }
static inline void     op_wr(uint32_t o, uint32_t v) { mmio_write32(xh.op, o, v); }
static inline void     rt_wr(uint32_t o, uint32_t v) { mmio_write32(xh.rt, 0x20 + o, v); }
static void op_wr64(uint32_t o, uint64_t v) { op_wr(o, (uint32_t)v); op_wr(o + 4, (uint32_t)(v >> 32)); }
static void rt_wr64(uint32_t o, uint64_t v) { rt_wr(o, (uint32_t)v); rt_wr(o + 4, (uint32_t)(v >> 32)); }

/* context entry size in bytes */
static uint32_t ctxsz(void) { return xh.csz ? 64 : 32; }

/* --- ring helpers --------------------------------------------------------- */
static int ring_init(struct ring *r) {
    if (dma_alloc(RING_SZ * 16, &r->buf) != 0) return -1;
    r->trb   = (volatile uint32_t *)r->buf.virt;
    r->phys  = r->buf.phys;
    r->enq   = 0;
    r->cycle = 1;
    /* last TRB is a Link back to the start, with Toggle Cycle set */
    volatile uint32_t *l = r->trb + (RING_SZ - 1) * 4;
    l[0] = (uint32_t)r->phys;
    l[1] = (uint32_t)(r->phys >> 32);
    l[2] = 0;
    l[3] = (TRB_LINK << 10) | (1u << 1);   /* TC=1 */
    return 0;
}

static uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile uint32_t *t = r->trb + r->enq * 4;
    uint64_t addr = r->phys + r->enq * 16;
    t[0] = d0; t[1] = d1; t[2] = d2;
    __asm__ volatile("" ::: "memory");
    t[3] = (d3 & ~1u) | r->cycle;          /* set cycle last */
    r->enq++;
    if (r->enq == RING_SZ - 1) {
        volatile uint32_t *l = r->trb + (RING_SZ - 1) * 4;
        l[3] = (l[3] & ~1u) | r->cycle;    /* arm the link TRB */
        r->enq = 0;
        r->cycle ^= 1;
    }
    return addr;
}

/* Poll the event ring for the next event. Fills cc/slot/ptr/type. Returns 0 on
 * an event, -1 on timeout. Advances ERDP. */
static int evt_poll(uint32_t *cc, uint32_t *slot, uint64_t *ptr, uint32_t *type) {
    for (uint64_t t = 0; t < POLL_MAX; t++) {
        volatile uint32_t *e = xh.evt.trb + xh.evt_deq * 4;
        uint32_t d3 = e[3];
        if ((d3 & 1u) == xh.evt_cycle) {
            if (type) *type = (d3 >> 10) & 0x3F;
            if (cc)   *cc   = (e[2] >> 24) & 0xFF;
            if (slot) *slot = (d3 >> 24) & 0xFF;
            if (ptr)  *ptr  = (uint64_t)e[0] | ((uint64_t)e[1] << 32);
            xh.evt_deq++;
            if (xh.evt_deq == RING_SZ) { xh.evt_deq = 0; xh.evt_cycle ^= 1; }
            uint64_t erdp = xh.evt.phys + xh.evt_deq * 16;
            rt_wr64(IR0_ERDP, erdp | (1u << 3));   /* EHB: clear event handler busy */
            return 0;
        }
    }
    return -1;
}

/* Issue a command-ring TRB and wait for its Command Completion Event. Returns
 * completion code, and the slot id via *out_slot. -1 on timeout. */
static int cmd_exec(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t *out_slot) {
    ring_push(&xh.cmd, d0, d1, d2, d3);
    __asm__ volatile("" ::: "memory");
    xh.db[0] = 0;                                  /* ring command doorbell */
    uint32_t cc = 0, slot = 0, type = 0; uint64_t p = 0;
    for (int guard = 0; guard < 8; guard++) {
        if (evt_poll(&cc, &slot, &p, &type) != 0) return -1;
        if (type == TRB_CMD_EVENT) { if (out_slot) *out_slot = slot; return (int)cc; }
        /* swallow port-status-change events that arrive between commands */
    }
    return -1;
}

/* --- EP0 control transfer ------------------------------------------------- */
/* tr = the device's EP0 transfer ring; setup the 8-byte request; dir_in=1 for
 * data stage device->host. buf/len describe the data stage (len 0 -> none). */
static int control_xfer(struct ring *tr, uint8_t slot,
                        uint8_t bmReqType, uint8_t bReq, uint16_t wValue,
                        uint16_t wIndex, void *buf, uint16_t len) {
    uint32_t trt = len ? (bmReqType & 0x80 ? 3u : 2u) : 0u;   /* IN=3 OUT=2 none=0 */
    /* Setup stage (IDT: data inline in the TRB) */
    ring_push(tr,
        (uint32_t)bmReqType | ((uint32_t)bReq << 8) | ((uint32_t)wValue << 16),
        (uint32_t)wIndex | ((uint32_t)len << 16),
        8,
        (1u << 6) | (TRB_SETUP << 10) | (trt << 16));
    /* Data stage */
    if (len) {
        struct dma_buf db; if (dma_alloc(len, &db) != 0) return -1;
        if (!(bmReqType & 0x80)) memcpy(db.virt, buf, len);
        ring_push(tr,
            (uint32_t)db.phys, (uint32_t)(db.phys >> 32),
            len,
            (TRB_DATA << 10) | ((bmReqType & 0x80 ? 1u : 0u) << 16));
        /* Status stage: opposite direction, raise IOC */
        ring_push(tr, 0, 0, 0,
            (TRB_STATUS << 10) | (1u << 5) | ((bmReqType & 0x80 ? 0u : 1u) << 16));
        __asm__ volatile("" ::: "memory");
        xh.db[slot] = 1;                           /* DCI 1 = EP0 */
        uint32_t cc = 0, type = 0;
        if (evt_poll(&cc, NULL, NULL, &type) != 0) { dma_free(&db); return -1; }
        if (type != TRB_TR_EVENT || (cc != CC_SUCCESS && cc != CC_SHORT_PACKET)) {
            dma_free(&db); return -1;
        }
        if (bmReqType & 0x80) memcpy(buf, db.virt, len);
        dma_free(&db);
        return 0;
    }
    /* no data: status IN */
    ring_push(tr, 0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | (1u << 16));
    __asm__ volatile("" ::: "memory");
    xh.db[slot] = 1;
    uint32_t cc = 0, type = 0;
    if (evt_poll(&cc, NULL, NULL, &type) != 0) return -1;
    return (type == TRB_TR_EVENT && (cc == CC_SUCCESS || cc == CC_SHORT_PACKET)) ? 0 : -1;
}

static uint32_t mps0_for_speed(uint8_t speed) {
    switch (speed) {
        case 2:  return 8;    /* Low  */
        case 1:  return 8;    /* Full (QEMU HID is FS, bMaxPacketSize0=8) */
        case 3:  return 64;   /* High */
        case 4:  return 512;  /* Super */
        default: return 8;
    }
}

/* Reset one root-hub port (0-based). Returns 1 if a device is connected and
 * the port enabled, 0 otherwise. */
static int port_reset(uint32_t p) {
    uint32_t sc = op_rd(OP_PORTSC(p));
    if (!(sc & PORTSC_CCS)) return 0;              /* nothing attached */
    /* power on if needed */
    if (!(sc & PORTSC_PP)) {
        op_wr(OP_PORTSC(p), (sc & ~PORTSC_RW1CS) | PORTSC_PP);
    }
    /* assert reset (write 1 to PR), preserving non-change bits */
    sc = op_rd(OP_PORTSC(p)) & ~PORTSC_RW1CS;
    op_wr(OP_PORTSC(p), sc | PORTSC_PR);
    for (uint64_t t = 0; t < POLL_MAX; t++) {
        uint32_t s = op_rd(OP_PORTSC(p));
        if (s & PORTSC_PRC) {                      /* reset complete */
            op_wr(OP_PORTSC(p), (s & ~PORTSC_RW1CS) | PORTSC_PRC);  /* clear PRC */
            return (s & PORTSC_PED) ? 1 : 0;
        }
    }
    return 0;
}

/* Build the input context for Address Device and enumerate one device. */
static void enumerate_port(uint32_t p) {
    if (!port_reset(p)) return;
    uint32_t sc = op_rd(OP_PORTSC(p));
    uint8_t speed = (sc >> 10) & 0xF;

    /* Enable Slot */
    uint32_t slot = 0;
    int cc = cmd_exec(0, 0, 0, (TRB_ENABLE_SLOT << 10), &slot);
    if (cc != CC_SUCCESS || slot == 0 || slot > xh.maxslots) {
        kprintf("[xhci] port %u: enable slot failed (cc=%u)\n", p + 1, cc);
        return;
    }

    /* Device context (output) -> DCBAA[slot] */
    struct dma_buf devctx;
    if (dma_alloc(ctxsz() * 32, &devctx) != 0) return;
    xh.dcbaa[slot] = devctx.phys;

    /* EP0 transfer ring */
    struct ring *tr = (struct ring *)0;
    static struct ring ep0_rings[MAX_DEV];
    if (xh.ndev >= MAX_DEV) return;
    tr = &ep0_rings[xh.ndev];
    if (ring_init(tr) != 0) return;

    /* Input context: ICC + slot + EP0, each ctxsz() bytes */
    struct dma_buf inctx;
    if (dma_alloc(ctxsz() * 3, &inctx) != 0) return;
    memset(inctx.virt, 0, ctxsz() * 3);
    volatile uint32_t *icc  = (volatile uint32_t *)inctx.virt;
    volatile uint32_t *sctx = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz());
    volatile uint32_t *ep0  = (volatile uint32_t *)((uint8_t *)inctx.virt + ctxsz() * 2);
    icc[1] = 0x3;                                  /* Add A0 (slot) | A1 (EP0) */
    sctx[0] = ((uint32_t)speed << 20) | (1u << 27);/* speed, context entries=1 */
    sctx[1] = ((uint32_t)(p + 1)) << 16;           /* root hub port number     */
    uint32_t mps = mps0_for_speed(speed);
    ep0[1] = (4u << 3) | (3u << 1) | (mps << 16);  /* EP type=control, CErr=3  */
    ep0[2] = (uint32_t)(tr->phys) | 1u;            /* TR dequeue | DCS=1        */
    ep0[3] = (uint32_t)(tr->phys >> 32);

    /* Address Device (BSR=0) */
    cc = cmd_exec((uint32_t)inctx.phys, (uint32_t)(inctx.phys >> 32), 0,
                  (TRB_ADDRESS_DEV << 10) | (slot << 24), NULL);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] port %u slot %u: address device failed (cc=%u)\n", p + 1, slot, cc);
        return;
    }

    /* GET_DESCRIPTOR(device, 18) */
    uint8_t desc[18]; memset(desc, 0, sizeof desc);
    if (control_xfer(tr, slot, 0x80, 6, 0x0100, 0, desc, 18) != 0) {
        kprintf("[xhci] port %u slot %u: get device descriptor failed\n", p + 1, slot);
        return;
    }

    struct usbdev *d = &xh.dev[xh.ndev];
    d->used = 1; d->slot = slot; d->port = p + 1; d->speed = speed;
    d->dev_class = desc[4];
    d->vendor  = (uint16_t)desc[8]  | ((uint16_t)desc[9]  << 8);
    d->product = (uint16_t)desc[10] | ((uint16_t)desc[11] << 8);
    d->if_class = 0;

    /* GET_DESCRIPTOR(config, header then full) to read the interface class */
    uint8_t cfg[64]; memset(cfg, 0, sizeof cfg);
    if (control_xfer(tr, slot, 0x80, 6, 0x0200, 0, cfg, 9) == 0) {
        uint16_t total = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
        if (total > sizeof cfg) total = sizeof cfg;
        if (total > 9 && control_xfer(tr, slot, 0x80, 6, 0x0200, 0, cfg, total) == 0) {
            /* walk descriptors for the first interface (bDescriptorType==4) */
            uint32_t i = 0;
            while (i + 1 < total) {
                uint8_t blen = cfg[i], btype = cfg[i + 1];
                if (blen == 0) break;
                if (btype == 4 && i + 5 < total) { d->if_class = cfg[i + 5]; break; }
                i += blen;
            }
        }
    }

    const char *kind = "device";
    uint8_t kc = d->dev_class ? d->dev_class : d->if_class;
    if (kc == 3) kind = "HID";
    else if (kc == 8) kind = "mass-storage";
    else if (kc == 9) kind = "hub";
    else if (kc == 2) kind = "comm";
    kprintf("[xhci] port %u slot %u: %s %x:%x class %x speed %u\n",
            p + 1, slot, kind, d->vendor, d->product, kc, speed);
    xh.ndev++;
}

void xhci_init(void) {
    struct pci_dev pd;
    if (!pci_find_by_class(0x0C, 0x03, &pd)) return;
    if (pd.prog_if != 0x30) return;                /* 30h = xHCI */

    struct pci_bar bar;
    if (pci_bar(&pd, 0, &bar) != 0 || !bar.is_mmio || !pci_map_bar(&bar)) {
        kprintf("[xhci] BAR0 map failed\n");
        return;
    }
    xh.cap = bar.virt;
    pci_enable_bus_master(&pd);
    pci_write16(&pd, 0x04, pci_read16(&pd, 0x04) | (1u << 10));  /* Interrupt-Disable */

    uint32_t caplen  = cap_rd(CAP_CAPLENGTH) & 0xFF;
    uint32_t hcs1    = cap_rd(CAP_HCSPARAMS1);
    uint32_t hcc1    = cap_rd(CAP_HCCPARAMS1);
    uint32_t dboff   = cap_rd(CAP_DBOFF) & ~0x3u;
    uint32_t rtsoff  = cap_rd(CAP_RTSOFF) & ~0x1Fu;
    xh.op = (volatile uint8_t *)xh.cap + caplen;
    xh.rt = (volatile uint8_t *)xh.cap + rtsoff;
    xh.db = (volatile uint32_t *)((volatile uint8_t *)xh.cap + dboff);
    xh.maxslots = hcs1 & 0xFF;
    xh.maxports = (hcs1 >> 24) & 0xFF;
    xh.csz = (hcc1 >> 2) & 1;

    /* reset: stop, then HCRST, wait for HCRST clear and CNR clear */
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) & ~USBCMD_RS);
    for (uint64_t t = 0; t < POLL_MAX; t++) if (op_rd(OP_USBSTS) & USBSTS_HCH) break;
    op_wr(OP_USBCMD, USBCMD_HCRST);
    for (uint64_t t = 0; t < POLL_MAX; t++) if (!(op_rd(OP_USBCMD) & USBCMD_HCRST)) break;
    for (uint64_t t = 0; t < POLL_MAX; t++) if (!(op_rd(OP_USBSTS) & USBSTS_CNR)) break;

    /* DCBAA */
    if (dma_alloc((xh.maxslots + 1) * 8, &xh.dcbaa_buf) != 0) { kprintf("[xhci] DCBAA OOM\n"); return; }
    xh.dcbaa = (volatile uint64_t *)xh.dcbaa_buf.virt;

    /* scratchpad buffers, if the controller needs them */
    uint32_t hcs2 = cap_rd(CAP_HCSPARAMS2);
    uint32_t spb  = ((hcs2 >> 21) & 0x1F) | (((hcs2 >> 27) & 0x1F) << 5);
    if (spb) {
        struct dma_buf arr; if (dma_alloc(spb * 8, &arr) != 0) { kprintf("[xhci] scratch OOM\n"); return; }
        volatile uint64_t *a = (volatile uint64_t *)arr.virt;
        for (uint32_t i = 0; i < spb; i++) {
            struct dma_buf pg; if (dma_alloc(4096, &pg) != 0) { kprintf("[xhci] scratch pg OOM\n"); return; }
            a[i] = pg.phys;
        }
        xh.dcbaa[0] = arr.phys;
    }
    op_wr64(OP_DCBAAP, xh.dcbaa_buf.phys);
    op_wr(OP_CONFIG, xh.maxslots);

    /* command ring */
    if (ring_init(&xh.cmd) != 0) { kprintf("[xhci] cmd ring OOM\n"); return; }
    op_wr64(OP_CRCR, xh.cmd.phys | 1u);            /* RCS=1 */

    /* event ring: single segment + ERST */
    if (ring_init(&xh.evt) != 0) { kprintf("[xhci] evt ring OOM\n"); return; }
    /* event ring has no link TRB; clear the one ring_init armed */
    xh.evt.trb[(RING_SZ - 1) * 4 + 3] = 0;
    xh.evt_cycle = 1; xh.evt_deq = 0;
    struct dma_buf erst; if (dma_alloc(16, &erst) != 0) { kprintf("[xhci] ERST OOM\n"); return; }
    volatile uint32_t *e = (volatile uint32_t *)erst.virt;
    e[0] = (uint32_t)xh.evt.phys;
    e[1] = (uint32_t)(xh.evt.phys >> 32);
    e[2] = RING_SZ;                                /* ring segment size */
    e[3] = 0;
    rt_wr(IR0_ERSTSZ, 1);
    rt_wr64(IR0_ERDP, xh.evt.phys);
    rt_wr64(IR0_ERSTBA, erst.phys);
    rt_wr(IR0_IMAN, 0);                            /* keep interrupts disabled */

    /* run */
    op_wr(OP_USBCMD, op_rd(OP_USBCMD) | USBCMD_RS);
    for (uint64_t t = 0; t < POLL_MAX; t++) if (!(op_rd(OP_USBSTS) & USBSTS_HCH)) break;

    xh.present = 1;
    kprintf("[xhci] %x:%x up: %u slots, %u ports, %u-byte ctx\n",
            pd.vendor, pd.device, xh.maxslots, xh.maxports, ctxsz());

    for (uint32_t p = 0; p < xh.maxports; p++) enumerate_port(p);

    if (xh.ndev == 0) kprintf("[xhci] no devices attached\n");
}
