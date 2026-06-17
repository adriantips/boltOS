#include <stdint.h>
#include "driver.h"
#include "hw.h"
#include "kprintf.h"

/* ===========================================================================
 *  Driver table walk. The linker collects DRIVER_REGISTER pointers between
 *  __drivers_start and __drivers_end (see linker.ld). We enumerate each bus and
 *  give every device to the first registered driver that claims it.
 * ===========================================================================*/
extern const struct driver *const __drivers_start[];
extern const struct driver *const __drivers_end[];

int driver_match(const struct driver *d, const struct device *dev) {
    if (d->bus != dev->bus) return 0;
    if (dev->bus != BUS_PCI) return 0;          /* only PCI ids defined so far */
    for (int i = 0; i < d->nids; i++) {
        const struct dev_id *id = &d->ids[i];
        if (id->match_class) {
            if ((id->class == DRV_ANY8 || id->class == dev->pci.class) &&
                (id->subclass == DRV_ANY8 || id->subclass == dev->pci.subclass))
                return 1;
        } else if ((id->vendor == DRV_ANY16 || id->vendor == dev->pci.vendor) &&
                   (id->device == DRV_ANY16 || id->device == dev->pci.device)) {
            return 1;
        }
    }
    return 0;
}

static int probe_pci(void) {
    struct pci_dev pds[64];
    int n = pci_scan(pds, 64);
    int claimed = 0;
    for (int i = 0; i < n; i++) {
        struct device dev = { .bus = BUS_PCI, .pci = pds[i] };
        for (const struct driver *const *pp = __drivers_start; pp < __drivers_end; pp++) {
            const struct driver *d = *pp;
            if (!driver_match(d, &dev)) continue;
            if (d->probe(&dev) == 0) { claimed++; break; }   /* first claim wins */
        }
    }
    return claimed;
}

int driver_probe_all(void) {
    int ndrv = (int)(__drivers_end - __drivers_start);
    kprintf("[drv] %d driver(s) registered; probing PCI\n", ndrv);
    int claimed = probe_pci();
    /* BUS_USB drivers (ath9k-htc / P5) wait on a USB host stack -- not built. */
    return claimed;
}
