#include <stdint.h>
#include <stddef.h>
#include "hw.h"
#include "blk.h"
#include "dma.h"
#include "mmio.h"
#include "string.h"
#include "kprintf.h"
#include "interrupts.h"

/* ===========================================================================
 *  NVMe controller driver (interrupt-driven).
 *
 *  Brings up the admin queue pair, identifies namespace 1, creates one I/O
 *  queue pair, and registers the namespace with the generic block layer.
 *
 *  Completions are delivered by a hardware MSI/MSI-X interrupt: the I/O CQ is
 *  created with IEN=1 (interrupt enable, vector 0) and the controller's MSI-X
 *  vector 0 is routed to an IDT vector (msi_install). The issuing thread halts
 *  (hlt) until the interrupt wakes it, then reads the single outstanding CQE --
 *  it no longer busy-polls the phase bit. If the controller exposes no MSI
 *  capability the driver falls back to polling so it still works everywhere.
 *
 *  Data transfers use a single page-aligned bounce buffer and are chunked to
 *  <= 4 KiB per command, so PRP1 alone describes the transfer (PRP2 unused)
 *  and never crosses a page boundary.
 * ===========================================================================*/

/* controller registers (BAR0 offsets) */
#define REG_CAP    0x00   /* 64-bit capabilities          */
#define REG_VS     0x08
#define REG_CC     0x14   /* controller configuration     */
#define REG_INTMS  0x0C   /* interrupt mask set           */
#define REG_CSTS   0x1C   /* controller status            */
#define REG_AQA    0x24   /* admin queue attributes       */
#define REG_ASQ    0x28   /* admin SQ base (64-bit)       */
#define REG_ACQ    0x30   /* admin CQ base (64-bit)       */
#define REG_DBS    0x1000 /* doorbell base                */

#define CC_EN      (1u << 0)
#define CSTS_RDY   (1u << 0)
#define CSTS_CFS   (1u << 1)

#define ADMIN_OP_CREATE_SQ  0x01
#define ADMIN_OP_CREATE_CQ  0x05
#define ADMIN_OP_IDENTIFY   0x06
#define IO_OP_WRITE         0x01
#define IO_OP_READ          0x02

#define QDEPTH   8

struct nvme_queue {
    volatile uint32_t *sqes;   /* submission entries (16 dwords each) */
    volatile uint32_t *cqes;   /* completion entries (4 dwords each)  */
    uint64_t sq_phys, cq_phys;
    uint32_t sq_tail, cq_head;
    uint32_t phase;            /* expected phase bit                  */
    uint32_t sq_db, cq_db;     /* doorbell register offsets           */
    struct dma_buf sq_buf, cq_buf;
};

static struct {
    int present;
    int irq_ok;                /* 1 once MSI/MSI-X delivers completions */
    volatile void *regs;
    uint32_t dstrd;
    struct nvme_queue admin, io;
    struct dma_buf bounce;     /* one page, page-aligned              */
    uint64_t nsze;             /* namespace size in logical blocks    */
    uint32_t lbads;            /* logical block size in bytes         */
    uint16_t cid;
} nv;

#define NVME_MSI_VECTOR  (MSI_VECTOR_BASE + 1)   /* 0x71 */

/* The completion interrupt fires here. With one command outstanding at a time
 * the only job is to wake the halted submit() loop, which then reads the CQE;
 * a tiny counter aids diagnostics. isr_handler issues the local-APIC EOI. */
static volatile uint64_t nvme_irq_count;
static void nvme_irq(struct registers *r) { (void)r; nvme_irq_count++; }

/* Only park on hlt when interrupts are actually unmasked, so a caller that
 * happens to hold IF=0 can never wedge the machine waiting for a wakeup. */
static inline int ints_on(void) {
    uint64_t f; __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return (int)((f >> 9) & 1u);
}

static inline uint32_t rd32(uint32_t off)            { return mmio_read32(nv.regs, off); }
static inline void     wr32(uint32_t off, uint32_t v){ mmio_write32(nv.regs, off, v); }
static void wr64(uint32_t off, uint64_t v) {          /* low then high dword */
    wr32(off, (uint32_t)v);
    wr32(off + 4, (uint32_t)(v >> 32));
}

static int alloc_queue(struct nvme_queue *q, uint32_t qid) {
    if (dma_alloc(QDEPTH * 64, &q->sq_buf) != 0) return -1;
    if (dma_alloc(QDEPTH * 16, &q->cq_buf) != 0) return -1;
    q->sqes    = (volatile uint32_t *)q->sq_buf.virt;
    q->cqes    = (volatile uint32_t *)q->cq_buf.virt;
    q->sq_phys = q->sq_buf.phys;
    q->cq_phys = q->cq_buf.phys;
    q->sq_tail = q->cq_head = 0;
    q->phase   = 1;
    uint32_t stride = 4u << nv.dstrd;
    q->sq_db = REG_DBS + (2 * qid)     * stride;
    q->cq_db = REG_DBS + (2 * qid + 1) * stride;
    return 0;
}

/* Submit a 16-dword command and wait for its completion. In interrupt mode the
 * CPU halts between checks and the MSI wakes it; the timer tick is a backstop so
 * a missed interrupt can never hang forever. In polled mode (no MSI capability)
 * it spins on the phase bit as before. Returns the NVMe status (0 == success),
 * or -1 on timeout. Only this function advances cq_head/phase, so the lone
 * outstanding completion is consumed exactly once with no IRQ-side race. */
static int submit(struct nvme_queue *q, const uint32_t cmd[16]) {
    uint16_t cid = nv.cid++;
    volatile uint32_t *e = q->sqes + q->sq_tail * 16;
    for (int i = 0; i < 16; i++) e[i] = cmd[i];
    e[0] = (e[0] & 0x0000FFFFu) | ((uint32_t)cid << 16);   /* CDW0.CID */

    q->sq_tail = (q->sq_tail + 1) % QDEPTH;
    __asm__ volatile("" ::: "memory");
    wr32(q->sq_db, q->sq_tail);

    volatile uint32_t *c = q->cqes + q->cq_head * 4;
    for (uint64_t t = 0; t < 5000000ull; t++) {
        uint32_t dw3 = c[3];
        if (((dw3 >> 16) & 1u) == q->phase) {              /* phase flipped */
            uint32_t status = (dw3 >> 17) & 0x7FFu;
            q->cq_head = (q->cq_head + 1) % QDEPTH;
            if (q->cq_head == 0) q->phase ^= 1;            /* wrap toggles phase */
            __asm__ volatile("" ::: "memory");
            wr32(q->cq_db, q->cq_head);
            return (int)status;
        }
        /* Interrupt-driven: sleep until the completion MSI (or the periodic
         * timer) wakes us, instead of burning the core on the phase bit. */
        if (nv.irq_ok && ints_on()) __asm__ volatile("hlt");
        else                        __asm__ volatile("pause");
    }
    kprintf("[nvme] submit timeout: sq_tail=%u cq_head=%u phase=%u dw3=%x\n",
            q->sq_tail, q->cq_head, q->phase, c[3]);
    return -1;
}

/* --- block-layer read/write ----------------------------------------------- */
static int nvme_rw(uint64_t lba, uint32_t count, void *buf, int write) {
    uint8_t *p = buf;
    uint32_t per = 4096 / nv.lbads;                        /* sectors per chunk */
    while (count) {
        uint32_t n = count < per ? count : per;
        if (write) memcpy(nv.bounce.virt, p, n * nv.lbads);

        uint32_t cmd[16]; memset(cmd, 0, sizeof cmd);
        cmd[0]  = write ? IO_OP_WRITE : IO_OP_READ;        /* CDW0 opcode      */
        cmd[1]  = 1;                                       /* CDW1 NSID = 1    */
        cmd[6]  = (uint32_t)nv.bounce.phys;                /* PRP1 low         */
        cmd[7]  = (uint32_t)(nv.bounce.phys >> 32);        /* PRP1 high        */
        cmd[10] = (uint32_t)lba;                           /* CDW10 SLBA low   */
        cmd[11] = (uint32_t)(lba >> 32);                   /* CDW11 SLBA high  */
        cmd[12] = (n - 1) & 0xFFFFu;                       /* CDW12 NLB (0-based) */

        if (submit(&nv.io, cmd) != 0) return -1;
        if (!write) memcpy(p, nv.bounce.virt, n * nv.lbads);

        lba   += n;
        p     += n * nv.lbads;
        count -= n;
    }
    return 0;
}
static int nvme_blk_read(blkdev_t *b, uint64_t lba, uint32_t count, void *buf) {
    (void)b; return nvme_rw(lba, count, buf, 0);
}
static int nvme_blk_write(blkdev_t *b, uint64_t lba, uint32_t count, const void *buf) {
    (void)b; return nvme_rw(lba, count, (void *)(uintptr_t)buf, 1);
}

static int identify_ns(void) {
    struct dma_buf id;
    if (dma_alloc(4096, &id) != 0) return -1;
    uint32_t cmd[16]; memset(cmd, 0, sizeof cmd);
    cmd[0]  = ADMIN_OP_IDENTIFY;
    cmd[1]  = 1;                                /* NSID = 1            */
    cmd[6]  = (uint32_t)id.phys;
    cmd[7]  = (uint32_t)(id.phys >> 32);
    cmd[10] = 0;                                /* CNS = 0: namespace  */
    if (submit(&nv.admin, cmd) != 0) { dma_free(&id); return -1; }

    uint8_t *d = id.virt;
    nv.nsze = *(uint64_t *)(d + 0);             /* NSZE @ byte 0       */
    uint8_t flbas = d[26] & 0x0F;               /* current LBA format  */
    /* LBA Format entries start at byte 128, 4 bytes each; LBADS is bits 23:16 */
    uint32_t fmt = *(uint32_t *)(d + 128 + flbas * 4);
    uint8_t lbads_log = (fmt >> 16) & 0xFF;
    nv.lbads = 1u << lbads_log;
    if (nv.lbads < 512) nv.lbads = 512;
    dma_free(&id);
    return 0;
}

static int create_io_queues(void) {
    if (alloc_queue(&nv.io, 1) != 0) return -1;

    uint32_t cmd[16];
    /* Create I/O Completion Queue (qid 1) */
    memset(cmd, 0, sizeof cmd);
    cmd[0]  = ADMIN_OP_CREATE_CQ;
    cmd[6]  = (uint32_t)nv.io.cq_phys;
    cmd[7]  = (uint32_t)(nv.io.cq_phys >> 32);
    cmd[10] = ((QDEPTH - 1) << 16) | 1;         /* QSIZE-1, QID=1      */
    cmd[11] = 1u | (nv.irq_ok ? 2u : 0u);       /* PC=1, IEN=irq, IV=0 */
    if (submit(&nv.admin, cmd) != 0) return -1;

    /* Create I/O Submission Queue (qid 1, bound to CQ 1) */
    memset(cmd, 0, sizeof cmd);
    cmd[0]  = ADMIN_OP_CREATE_SQ;
    cmd[6]  = (uint32_t)nv.io.sq_phys;
    cmd[7]  = (uint32_t)(nv.io.sq_phys >> 32);
    cmd[10] = ((QDEPTH - 1) << 16) | 1;         /* QSIZE-1, QID=1      */
    cmd[11] = (1u << 16) | 1;                   /* CQID=1, PC=1        */
    if (submit(&nv.admin, cmd) != 0) return -1;
    return 0;
}

void nvme_init(void) {
    struct pci_dev pd;
    if (!pci_find_by_class(0x01, 0x08, &pd)) return;       /* class 01h sub 08h = NVM */

    struct pci_bar bar;
    if (pci_bar(&pd, 0, &bar) != 0 || !bar.is_mmio || !pci_map_bar(&bar)) {
        kprintf("[nvme] BAR0 map failed\n");
        return;
    }
    nv.regs = bar.virt;
    pci_enable_bus_master(&pd);

    /* Route completions through a hardware MSI/MSI-X interrupt (the preferred
     * path). If the controller exposes no MSI capability, fall back to polling
     * and silence INTx (PCI command bit 10) so an unhandled pin assertion can't
     * storm the IDT -- the same defence the old polled driver relied on. */
    if (pci_msi_enable(&pd, NVME_MSI_VECTOR) == 0) {
        msi_install(NVME_MSI_VECTOR, nvme_irq);
        nv.irq_ok = 1;
    } else {
        pci_write16(&pd, 0x04, pci_read16(&pd, 0x04) | (1u << 10));
    }

    uint64_t cap = (uint64_t)rd32(REG_CAP) | ((uint64_t)rd32(REG_CAP + 4) << 32);
    nv.dstrd = (cap >> 32) & 0xF;

    /* reset: clear CC.EN, wait until not ready */
    wr32(REG_CC, 0);
    for (uint64_t t = 0; t < 5000000ull; t++) { if (!(rd32(REG_CSTS) & CSTS_RDY)) break; }
    if (!nv.irq_ok) wr32(REG_INTMS, 0xFFFFFFFFu);   /* polled: mask completions */

    if (alloc_queue(&nv.admin, 0) != 0) { kprintf("[nvme] admin queue OOM\n"); return; }
    wr32(REG_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
    wr64(REG_ASQ, nv.admin.sq_phys);
    wr64(REG_ACQ, nv.admin.cq_phys);

    /* CC: IOSQES=6 (64B), IOCQES=4 (16B), MPS=0 (4KB), CSS=0, EN=1 */
    uint32_t cc = (6u << 16) | (4u << 20) | (0u << 7) | CC_EN;
    wr32(REG_CC, cc);

    int ready = 0;
    for (uint64_t t = 0; t < 5000000ull; t++) {
        uint32_t s = rd32(REG_CSTS);
        if (s & CSTS_CFS) { kprintf("[nvme] controller fatal status\n"); return; }
        if (s & CSTS_RDY) { ready = 1; break; }
    }
    if (!ready) { kprintf("[nvme] controller not ready\n"); return; }

    if (dma_alloc(4096, &nv.bounce) != 0) { kprintf("[nvme] bounce OOM\n"); return; }

    if (identify_ns() != 0)     { kprintf("[nvme] identify ns failed\n"); return; }
    if (create_io_queues() != 0){ kprintf("[nvme] create io queues failed\n"); return; }

    nv.present = 1;
    uint64_t mb = (nv.nsze * nv.lbads) / (1024 * 1024);
    kprintf("[nvme] %x:%x ns1 %luMiB %u-byte blocks (%s)\n",
            pd.vendor, pd.device, (unsigned long)mb, nv.lbads,
            nv.irq_ok ? "MSI irq" : "polled");

    blkdev_t b; memset(&b, 0, sizeof b);
    b.sectors     = nv.nsze;
    b.sector_size = nv.lbads;
    b.is_ssd      = 1;
    b.is_boot     = 0;
    b.read  = nvme_blk_read;
    b.write = nvme_blk_write;
    strncpy(b.name,  "nvme0n1",   sizeof b.name - 1);
    strncpy(b.model, "QEMU NVMe",  sizeof b.model - 1);
    blk_register(&b);
}
