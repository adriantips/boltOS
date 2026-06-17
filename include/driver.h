#pragma once
#include <stdint.h>
#include "hw.h"

/* ===========================================================================
 *  Generic driver/bus binding (P4).
 *
 *  Replaces the hardcoded e1000_init() call with a link-time driver table: each
 *  driver declares the bus it lives on and an id-match table, then DRIVER_REGISTER
 *  drops a pointer into the .drivers section. driver_probe_all() enumerates every
 *  bus and hands matching devices to a driver's probe(). No C constructors run in
 *  this freestanding kernel, so the linker collects the table for us.
 *
 *  BUS_PCI is the only bus with an enumerator today; BUS_USB is declared now so a
 *  softMAC WiFi part (ath9k-htc, P5) drops in behind the same match/probe seam
 *  once a USB host stack exists.
 * ===========================================================================*/
enum bus_type { BUS_PCI = 0, BUS_USB = 1 };

#define DRV_ANY16 0xFFFF
#define DRV_ANY8  0xFF

/* One match key. By default it matches PCI (vendor,device); set match_class to
 * bind by (class,subclass) instead. DRV_ANY16/DRV_ANY8 wildcard a field. */
struct dev_id {
    uint16_t vendor, device;
    uint8_t  class, subclass;
    uint8_t  match_class;        /* 1 = match class/subclass, not vendor/device */
};

/* A bus-bound device passed to probe(). For BUS_PCI, .pci is valid. */
struct device {
    enum bus_type  bus;
    struct pci_dev pci;          /* valid when bus == BUS_PCI */
};

/* probe() returns 0 if it claimed and initialised the device, <0 otherwise. */
struct driver {
    const char          *name;
    enum bus_type        bus;
    const struct dev_id *ids;
    int                  nids;
    int                (*probe)(struct device *dev);
};

/* Drop a driver descriptor pointer into the link-time table. */
#define DRIVER_REGISTER(drv) \
    static const struct driver *const __drvptr_##drv \
        __attribute__((used, section(".drivers"))) = &drv

/* 1 if device matches any of driver d's id keys (bus included). */
int driver_match(const struct driver *d, const struct device *dev);

/* Enumerate every bus and probe matching drivers. Returns devices claimed. */
int driver_probe_all(void);
