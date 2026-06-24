#pragma once
/* AHCI / SATA host controller. Probes the PCI AHCI controller, brings up each
 * SATA port, and registers disks with the generic block layer. Polled (no IRQ),
 * DMA data path -> ~100x the throughput of the legacy ATA PIO driver. */
void ahci_init(void);
