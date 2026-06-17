#include <stdint.h>
#include "hw.h"
#include "io.h"
#include "mmio.h"
#include "dma.h"
#include "netif.h"
#include "driver.h"
#include "interrupts.h"
#include "kprintf.h"

/* ===========================================================================
 *  Intel e1000 (82540EM) driver -- the NIC QEMU exposes with `-device e1000`
 *  (and its default `-net nic`). Legacy descriptor rings, INTx interrupt.
 *
 *  This is the first real consumer of the P0 (PCI BAR/bus-master) and P1 (DMA)
 *  layers and validates the whole data path under QEMU before any WiFi work.
 * ===========================================================================*/
#define E1000_VENDOR 0x8086
#define E1000_DEV_82540EM 0x100E    /* QEMU default */

/* MMIO registers */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200          /* 128 dwords */
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_SLU    0x00000040
#define CTRL_ASDE   0x00000020
#define CTRL_RST    0x04000000

#define RCTL_EN     0x00000002
#define RCTL_BAM    0x00008000      /* accept broadcast */
#define RCTL_SECRC  0x04000000      /* strip Ethernet CRC */
#define RCTL_BSIZE_2048 0x00000000  /* 2048-byte buffers (BSIZE=00, BSEX=0) */

#define TCTL_EN     0x00000002
#define TCTL_PSP    0x00000008
#define TCTL_CT     (0x10 << 4)     /* collision threshold */
#define TCTL_COLD   (0x40 << 12)    /* collision distance, full duplex */

/* interrupt cause/mask bits */
#define INT_TXDW    0x01
#define INT_LSC     0x04
#define INT_RXDMT0  0x10
#define INT_RXO     0x40
#define INT_RXT0    0x80

/* TX cmd / status */
#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08
#define TXD_STA_DD   0x01
/* RX status */
#define RXD_STA_DD   0x01
#define RXD_STA_EOP  0x02

#define RX_DESCS  32
#define TX_DESCS  8
#define BUF_SIZE  2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct e1000 {
    volatile void *regs;
    struct dma_buf rx_ring, tx_ring, rx_bufs, tx_bufs;
    struct rx_desc *rx;
    struct tx_desc *tx;
    uint8_t        *rxbuf;   /* RX_DESCS * BUF_SIZE */
    uint8_t        *txbuf;   /* TX_DESCS * BUF_SIZE */
    uint32_t        rx_cur, tx_cur;
    struct netif    nif;
};

static struct e1000 dev;

static inline uint32_t er(uint32_t r)            { return mmio_read32(dev.regs, r); }
static inline void     ew(uint32_t r, uint32_t v){ mmio_write32(dev.regs, r, v); }

static void read_mac(uint8_t mac[6]) {
    uint32_t ral = er(REG_RAL0);
    uint32_t rah = er(REG_RAH0);
    if (ral != 0) {
        mac[0] = ral & 0xFF; mac[1] = (ral >> 8) & 0xFF;
        mac[2] = (ral >> 16) & 0xFF; mac[3] = (ral >> 24) & 0xFF;
        mac[4] = rah & 0xFF; mac[5] = (rah >> 8) & 0xFF;
        return;
    }
    /* fallback: read words 0..2 from EEPROM via EERD */
    for (int i = 0; i < 3; i++) {
        ew(REG_EERD, ((uint32_t)i << 8) | 1u);     /* addr<<8 | START */
        uint32_t d = 0;
        for (int g = 0; g < 100000; g++) { d = er(REG_EERD); if (d & 0x10) break; }
        uint16_t w = (uint16_t)(d >> 16);
        mac[i * 2]     = w & 0xFF;
        mac[i * 2 + 1] = (w >> 8) & 0xFF;
    }
}

static void rx_init(void) {
    dma_alloc(RX_DESCS * sizeof(struct rx_desc), &dev.rx_ring);
    dma_alloc(RX_DESCS * BUF_SIZE, &dev.rx_bufs);
    dev.rx     = (struct rx_desc *)dev.rx_ring.virt;
    dev.rxbuf  = (uint8_t *)dev.rx_bufs.virt;
    for (int i = 0; i < RX_DESCS; i++) {
        dev.rx[i].addr   = dev.rx_bufs.phys + (uint64_t)i * BUF_SIZE;
        dev.rx[i].status = 0;
    }
    ew(REG_RDBAL, (uint32_t)(dev.rx_ring.phys & 0xFFFFFFFF));
    ew(REG_RDBAH, (uint32_t)(dev.rx_ring.phys >> 32));
    ew(REG_RDLEN, RX_DESCS * sizeof(struct rx_desc));
    ew(REG_RDH, 0);
    ew(REG_RDT, RX_DESCS - 1);
    dev.rx_cur = 0;
    ew(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void tx_init(void) {
    dma_alloc(TX_DESCS * sizeof(struct tx_desc), &dev.tx_ring);
    dma_alloc(TX_DESCS * BUF_SIZE, &dev.tx_bufs);
    dev.tx    = (struct tx_desc *)dev.tx_ring.virt;
    dev.txbuf = (uint8_t *)dev.tx_bufs.virt;
    for (int i = 0; i < TX_DESCS; i++) {
        dev.tx[i].addr   = dev.tx_bufs.phys + (uint64_t)i * BUF_SIZE;
        dev.tx[i].status = TXD_STA_DD;     /* mark free */
    }
    ew(REG_TDBAL, (uint32_t)(dev.tx_ring.phys & 0xFFFFFFFF));
    ew(REG_TDBAH, (uint32_t)(dev.tx_ring.phys >> 32));
    ew(REG_TDLEN, TX_DESCS * sizeof(struct tx_desc));
    ew(REG_TDH, 0);
    ew(REG_TDT, 0);
    dev.tx_cur = 0;
    ew(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    ew(REG_TIPG, 0x0060200A);
}

static void rx_drain(void) {
    while (dev.rx[dev.rx_cur].status & RXD_STA_DD) {
        struct rx_desc *d = &dev.rx[dev.rx_cur];
        uint16_t len = d->length;
        uint8_t *buf = dev.rxbuf + (uint64_t)dev.rx_cur * BUF_SIZE;
        netif_rx(&dev.nif, buf, len);
        d->status = 0;
        uint32_t old = dev.rx_cur;
        dev.rx_cur = (dev.rx_cur + 1) % RX_DESCS;
        ew(REG_RDT, old);            /* hand the buffer back to the NIC */
    }
}

static int e1000_send(struct netif *nif, const void *frame, uint16_t len) {
    (void)nif;
    if (len > BUF_SIZE) return -1;
    uint32_t i = dev.tx_cur;
    /* wait for the slot to drain (DD set) */
    for (int g = 0; g < 1000000 && !(dev.tx[i].status & TXD_STA_DD); g++) io_wait();

    uint8_t *b = dev.txbuf + (uint64_t)i * BUF_SIZE;
    for (uint16_t k = 0; k < len; k++) b[k] = ((const uint8_t *)frame)[k];

    dev.tx[i].length = len;
    dev.tx[i].cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    dev.tx[i].status = 0;
    dev.tx_cur = (i + 1) % TX_DESCS;
    ew(REG_TDT, dev.tx_cur);
    return 0;
}

static void e1000_poll(struct netif *nif) { (void)nif; rx_drain(); }

static void e1000_isr(struct registers *r) {
    (void)r;
    uint32_t icr = er(REG_ICR);     /* read clears the cause bits */
    if (icr & INT_LSC)
        dev.nif.link_up = (er(REG_STATUS) & 0x2) ? 1 : 0;
    if (icr & (INT_RXT0 | INT_RXDMT0 | INT_RXO))
        rx_drain();
}

static int e1000_probe(struct device *d) {
    struct pci_dev *pd = &d->pci;

    struct pci_bar bar;
    if (pci_bar(pd, 0, &bar) != 0 || !bar.is_mmio || !pci_map_bar(&bar)) {
        kprintf("[e1000] BAR0 map failed\n");
        return -1;
    }
    dev.regs = bar.virt;
    pci_enable_bus_master(pd);

    /* reset */
    ew(REG_IMC, 0xFFFFFFFF); (void)er(REG_ICR);
    ew(REG_CTRL, er(REG_CTRL) | CTRL_RST);
    for (int g = 0; g < 1000000 && (er(REG_CTRL) & CTRL_RST); g++) io_wait();
    ew(REG_IMC, 0xFFFFFFFF); (void)er(REG_ICR);

    ew(REG_CTRL, er(REG_CTRL) | CTRL_SLU | CTRL_ASDE);
    for (int i = 0; i < 128; i++) ew(REG_MTA + i * 4, 0);   /* clear multicast filter */

    read_mac(dev.nif.mac);

    /* set our unicast filter (RAL/RAH) with Address Valid */
    {
        const uint8_t *m = dev.nif.mac;
        ew(REG_RAL0, (uint32_t)m[0] | ((uint32_t)m[1] << 8) |
                     ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24));
        ew(REG_RAH0, (uint32_t)m[4] | ((uint32_t)m[5] << 8) | 0x80000000u);
    }

    rx_init();
    tx_init();

    dev.nif.link_up = (er(REG_STATUS) & 0x2) ? 1 : 0;
    dev.nif.send = e1000_send;
    dev.nif.poll = e1000_poll;
    dev.nif.drv  = &dev;
    dev.nif.name[0]='e'; dev.nif.name[1]='t'; dev.nif.name[2]='h';
    dev.nif.name[3]='0'; dev.nif.name[4]=0;

    /* wire the legacy INTx line and enable RX interrupts */
    uint8_t line = pci_interrupt_line(pd);
    irq_install(line, e1000_isr);
    ew(REG_IMS, INT_RXT0 | INT_RXDMT0 | INT_RXO | INT_LSC);
    (void)er(REG_ICR);

    kprintf("[e1000] 8086:100E @ PCI %x:%x.%x BAR0=0x%lx IRQ%u\n",
            pd->bus, pd->slot, pd->func, bar.phys, line);
    netif_register(&dev.nif);
    return 0;
}

/* Bind to the QEMU 82540EM via the P4 driver table. */
static const struct dev_id e1000_ids[] = {
    { .vendor = E1000_VENDOR, .device = E1000_DEV_82540EM },
};
static const struct driver e1000_driver = {
    .name  = "e1000",
    .bus   = BUS_PCI,
    .ids   = e1000_ids,
    .nids  = 1,
    .probe = e1000_probe,
};
DRIVER_REGISTER(e1000_driver);
