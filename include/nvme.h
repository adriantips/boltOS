#pragma once
/* NVMe controller driver (drivers/nvme.c). Probes the first NVM-class PCI
 * device, brings up admin + one I/O queue pair (polled), identifies namespace
 * 1, and registers it with the generic block layer (see include/blk.h). */
void nvme_init(void);
