/* ===========================================================================
 *  BoltOS  -  kernel/app_sysinfo.c
 *  System Info / About: a live dashboard summarising the machine - CPU brand,
 *  memory use, the persistent disk, network address, display size, uptime, and
 *  task/service counts. Read-only; refreshed from the window tick.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "hw.h"
#include "gpu.h"
#include "pmm.h"
#include "fs.h"
#include "net.h"
#include "pit.h"
#include "sysreg.h"
#include "string.h"
#include "commands.h"     /* sh_utoa, sh_human */

static char cpu_brand[49];

static void si_tick(window_t *w) { (void)w; gui_request_redraw(); }

static void fmt_ip(uint32_t ip, char *out, int cap) {
    char t[8]; out[0] = 0;
    for (int i = 3; i >= 0; i--) {
        sh_utoa((ip >> (i * 8)) & 0xFF, t);
        kstrlcat(out, t, cap);
        if (i) kstrlcat(out, ".", cap);
    }
}

/* one "label: value" row inside a card; returns next y */
static int row(int x, int y, int w, const char *label, const char *value) {
    g_text(x + 14, y, label, COL_TEXT_DIM, 1);
    int vw = g_text_width(value, 1);
    g_text(x + w - 14 - vw, y, value, COL_TEXT, 1);
    return y + 24;
}

static void si_draw(window_t *win, int cx, int cy, int cw, int ch) {
    (void)win; (void)ch;
    g_fill(cx, cy, cw, ch, COL_PANEL);

    int pad = 14, x = cx + pad, w = cw - 2 * pad, y = cy + pad;

    /* header card with the bolt logo */
    g_round(x, y, w, 70, 12, COL_PANEL_2, 255);
    gui_icon(ICON_START, x + 20, y + 18, 2, COL_ACCENT);
    g_text(x + 64, y + 16, "BoltOS", COL_TEXT, 3);
    g_text(x + 64, y + 46, "64-bit kernel  -  long mode", COL_TEXT_DIM, 1);
    y += 84;

    char buf[64], a[24], b[24];

    /* hardware card */
    int cardh = 24 * 6 + 16;
    g_round(x, y, w, cardh, 12, COL_PANEL_2, 255);
    int ry = y + 14;
    ry = row(x, ry, w, "Processor", cpu_brand[0] ? cpu_brand : "x86-64");

    uint64_t totp = pmm_total_count(), frep = pmm_free_count();
    uint64_t used = (totp - frep) * 4096, tot = totp * 4096;
    sh_human(used, a); sh_human(tot, b);
    buf[0] = 0; kstrlcat(buf, a, sizeof(buf)); kstrlcat(buf, " / ", sizeof(buf)); kstrlcat(buf, b, sizeof(buf));
    ry = row(x, ry, w, "Memory", buf);

    int dw = gui_screen_w(), dh = gui_screen_h();
    sh_utoa((uint64_t)dw, a); sh_utoa((uint64_t)dh, b);
    buf[0] = 0; kstrlcat(buf, a, sizeof(buf)); kstrlcat(buf, " x ", sizeof(buf)); kstrlcat(buf, b, sizeof(buf));
    ry = row(x, ry, w, "Display", buf);

    if (gpu_present()) {
        buf[0] = 0;
        kstrlcat(buf, gpu_name(), sizeof(buf));
        if (g_text_width(buf, 1) > w - 150) buf[20] = 0;   /* keep room for label */
        if (gpu_vram_bytes()) {
            sh_human(gpu_vram_bytes(), a);
            kstrlcat(buf, "  ", sizeof(buf)); kstrlcat(buf, a, sizeof(buf));
        }
    } else { buf[0] = 0; kstrlcat(buf, "none", sizeof(buf)); }
    ry = row(x, ry, w, "Graphics", buf);

    const char *media = fs_persist_media();
    buf[0] = 0;
    if (media && media[0]) {
        kstrlcat(buf, media, sizeof(buf));
        const char *model = fs_persist_model();
        if (model && model[0]) { kstrlcat(buf, "  ", sizeof(buf)); kstrlcat(buf, model, sizeof(buf)); }
    } else kstrlcat(buf, "RAM only", sizeof(buf));
    if (g_text_width(buf, 1) > w - 120) buf[18] = 0;       /* keep it on one line */
    ry = row(x, ry, w, "Storage", buf);

    sh_human(fs_total_bytes(), a); sh_utoa((uint64_t)fs_count_nodes(), b);
    buf[0] = 0; kstrlcat(buf, a, sizeof(buf)); kstrlcat(buf, " in ", sizeof(buf));
    kstrlcat(buf, b, sizeof(buf)); kstrlcat(buf, " files", sizeof(buf));
    ry = row(x, ry, w, "Filesystem", buf);
    y += cardh + 14;

    /* runtime card */
    int cardh2 = 24 * 3 + 16;
    g_round(x, y, w, cardh2, 12, COL_PANEL_2, 255);
    ry = y + 14;

    fmt_ip(net_ip, buf, sizeof(buf));
    if (!net_ip) { buf[0] = 0; kstrlcat(buf, "offline", sizeof(buf)); }
    ry = row(x, ry, w, "Network", buf);

    uint32_t hz = pit_hz() ? pit_hz() : 1000;
    uint64_t secs = pit_ticks() / hz;
    int hh = (int)(secs / 3600), mm = (int)((secs % 3600) / 60), ss = (int)(secs % 60);
    buf[0] = 0;
    sh_utoa((uint64_t)hh, a); kstrlcat(buf, a, sizeof(buf)); kstrlcat(buf, "h ", sizeof(buf));
    sh_utoa((uint64_t)mm, a); kstrlcat(buf, a, sizeof(buf)); kstrlcat(buf, "m ", sizeof(buf));
    sh_utoa((uint64_t)ss, a); kstrlcat(buf, a, sizeof(buf)); kstrlcat(buf, "s", sizeof(buf));
    ry = row(x, ry, w, "Uptime", buf);

    sh_utoa((uint64_t)task_count(), a); sh_utoa((uint64_t)svc_count(), b);
    buf[0] = 0; kstrlcat(buf, a, sizeof(buf)); kstrlcat(buf, " tasks  /  ", sizeof(buf));
    kstrlcat(buf, b, sizeof(buf)); kstrlcat(buf, " services", sizeof(buf));
    ry = row(x, ry, w, "Workload", buf);
}

void sysinfo_app_init(void) {
    hw_cpu_brand(cpu_brand);
    for (int i = (int)strlen(cpu_brand) - 1; i >= 0 && cpu_brand[i] == ' '; i--) cpu_brand[i] = 0;
    window_t *win = gui_add_window("System Info", 380, 410, 0x4F8DF7, ICON_SYSINFO);
    if (!win) return;
    win->draw = si_draw;
    win->tick = si_tick;
    win->min_w = 320; win->min_h = 360;
    win->x = 340; win->y = 90;
}
