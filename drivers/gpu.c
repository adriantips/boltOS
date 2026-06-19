#include <stdint.h>
#include "gpu.h"
#include "hw.h"
#include "io.h"
#include "framebuffer.h"
#include "kprintf.h"

/* ===========================================================================
 *  GPU / display-adapter driver  (see include/gpu.h)
 *
 *  Real device probe: walk PCI for a display controller, name it from its
 *  vendor:device id, decode BAR0 to size the VRAM aperture, and interrogate the
 *  Bochs DISPI (VBE) interface for its version id so we know runtime mode
 *  setting is available. Everything here reads real hardware registers (and
 *  works as-is under QEMU/VirtualBox std-VGA).
 * ===========================================================================*/

#define DISPI_INDEX 0x01CE
#define DISPI_DATA  0x01CF
#define DI_ID       0          /* index 0 -> VBE_DISPI_ID0..5 = 0xB0C0..0xB0C5 */

static struct {
    int      present;
    int      accel;            /* DISPI mode-set available             */
    uint16_t vendor, device;
    uint16_t dispi_id;
    uint64_t vram;             /* BAR0 aperture size                   */
    uint64_t fb_phys;
    char     name[48];
} g;

/* The resolutions Settings/GPU offers; all are valid 32bpp DISPI modes. */
static const struct gpu_mode modes[] = {
    {  800,  600 }, { 1024,  768 }, { 1280,  720 }, { 1280,  800 },
    { 1280, 1024 }, { 1366,  768 }, { 1440,  900 }, { 1600,  900 },
    { 1680, 1050 }, { 1920, 1080 },
};

static void set_name(const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(g.name) - 1; i++) g.name[i] = s[i];
    g.name[i] = 0;
}

/* Map a few well-known virtual/real display adapters to a friendly name. */
static void identify(uint16_t ven, uint16_t dev) {
    switch (ven) {
    case 0x1234: set_name("QEMU Standard VGA (Bochs DISPI)"); return;
    case 0x80EE: set_name("VirtualBox VGA (Bochs DISPI)");    return;
    case 0x1013: set_name("Cirrus Logic GD5446");             return;
    case 0x15AD: set_name("VMware SVGA II");                  return;
    case 0x1AF4: set_name("VirtIO GPU");                      return;
    case 0x8086: set_name("Intel Integrated Graphics");       return;
    case 0x10DE: set_name("NVIDIA GPU");                      return;
    case 0x1002: set_name("AMD/ATI GPU");                     return;
    default:     set_name("PCI Display Controller");          return;
    }
    (void)dev;
}

static uint16_t dispi_probe(void) {
    outw(DISPI_INDEX, DI_ID);
    uint16_t id = inw(DISPI_DATA);
    return (id >= 0xB0C0 && id <= 0xB0CF) ? id : 0;
}

int gpu_init(void) {
    for (int i = 0; i < (int)sizeof(g); i++) ((uint8_t *)&g)[i] = 0;

    struct pci_dev d;
    if (!pci_find_by_class(0x03, 0xFF, &d)) {
        kprintf("[--] GPU: no PCI display controller found\n");
        return 0;
    }
    g.present = 1;
    g.vendor  = d.vendor;
    g.device  = d.device;
    identify(d.vendor, d.device);

    /* BAR0 is the linear-framebuffer aperture on every adapter we name above;
     * its probed size is the visible VRAM window. */
    struct pci_bar bar;
    if (pci_bar(&d, 0, &bar) == 0 && bar.is_mmio) {
        g.vram    = bar.size;
        g.fb_phys = bar.phys;
    }

    g.dispi_id = dispi_probe();
    g.accel    = g.dispi_id != 0;

    kprintf("[ok] GPU: %s [%x:%x] %lu MiB VRAM%s\n",
            g.name, g.vendor, g.device, g.vram >> 20,
            g.accel ? ", DISPI modeset" : "");
    return 1;
}

int         gpu_present(void)      { return g.present; }
const char *gpu_name(void)         { return g.present ? g.name : "none"; }
uint16_t    gpu_vendor(void)       { return g.vendor; }
uint16_t    gpu_device(void)       { return g.device; }
uint64_t    gpu_vram_bytes(void)   { return g.vram; }
uint64_t    gpu_fb_phys(void)      { return g.fb_phys; }
int         gpu_accel(void)        { return g.accel; }
uint16_t    gpu_dispi_id(void)     { return g.dispi_id; }

int         gpu_mode_count(void)   { return (int)(sizeof(modes) / sizeof(modes[0])); }
const struct gpu_mode *gpu_modes(void) { return modes; }

int gpu_set_mode(uint32_t w, uint32_t h) {
    if (!g.accel) return -1;
    return fb_set_mode(w, h);
}
