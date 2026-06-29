/* ===========================================================================
 *  BoltOS  -  kernel/app_settings.c
 *  Settings window: live controls for the theme preset, accent colour,
 *  wallpaper style + colour, virtual resolution and aspect ratio. Each control
 *  registers a "hot rect" while drawing; the click handler hit-tests those and
 *  edits g_settings, then settings_apply() retints + relayouts the desktop.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "settings.h"
#include "commands.h"     /* sh_utoa */
#include "string.h"
#include "pcspk.h"
#include "pit.h"

/* control kinds */
enum { K_THEME = 1, K_ACCENT, K_WALLSTYLE, K_WALLCOLOR, K_RES, K_AUDIO, K_AUDIOTEST, K_FONT };

static uint64_t beep_off_at;      /* pit tick to silence the test tone, 0 = idle */

typedef struct { int x, y, w, h; uint8_t kind, val; } hot_t;

#define MAXHOT 64
static hot_t hots[MAXHOT];
static int   nhot;
static int   org_x, org_y;          /* client origin, so hots are client-local */

static void hot_add(int x, int y, int w, int h, int kind, int val) {
    if (nhot >= MAXHOT) return;
    hots[nhot].x = x - org_x; hots[nhot].y = y - org_y;
    hots[nhot].w = w; hots[nhot].h = h;
    hots[nhot].kind = (uint8_t)kind; hots[nhot].val = (uint8_t)val;
    nhot++;
}

/* ---- widgets ------------------------------------------------------------ */
static int section(int x, int y, const char *label) {
    g_fill(x, y, 3, 12, COL_ACCENT);
    g_text(x + 10, y + 2, label, COL_TEXT, 1);
    return y + 24;
}

static int chip(int x, int y, const char *label, int selected) {
    int w = g_text_width(label, 1) + 24, h = 30;
    g_round(x, y, w, h, 8, selected ? COL_ACCENT : COL_PANEL_3, 255);
    g_text(x + 12, y + 11, label, selected ? 0xFFFFFF : COL_TEXT, 1);
    return w;
}

static void swatch(int x, int y, int s, uint32_t color, int selected) {
    if (selected) {
        g_round(x, y, s, s, 7, COL_ACCENT, 255);
        g_round(x + 4, y + 4, s - 8, s - 8, 4, color, 255);
    } else {
        g_round(x, y, s, s, 7, color, 255);
    }
}

/* ---- layout helpers ----------------------------------------------------- */
/* a wrapping row of text chips */
static int chip_row(int x0, int right, int y, int kind, int count,
                    const char *(*name)(int), int sel) {
    int x = x0;
    for (int i = 0; i < count; i++) {
        const char *nm = name(i);
        int w = g_text_width(nm, 1) + 24;
        if (x != x0 && x + w > right) { x = x0; y += 38; }
        chip(x, y, nm, sel == i);
        hot_add(x, y, w, 30, kind, i);
        x += w + 8;
    }
    return y + 30 + 16;
}

/* a wrapping row of colour swatches; `sel_color` marks the selected one */
static int swatch_row(int x0, int right, int y, int kind, int count,
                      uint32_t (*color)(int), uint32_t sel_color) {
    int s = 30, x = x0;
    for (int i = 0; i < count; i++) {
        if (x != x0 && x + s > right) { x = x0; y += s + 8; }
        swatch(x, y, s, color(i), color(i) == sel_color);
        hot_add(x, y, s, s, kind, i);
        x += s + 8;
    }
    return y + s + 16;
}

static void fmt_dims(char *buf, int cap, int a, int b) {
    char t[12];
    buf[0] = 0;
    sh_utoa((uint64_t)a, t); kstrlcat(buf, t, cap);
    kstrlcat(buf, " x ", cap);
    sh_utoa((uint64_t)b, t); kstrlcat(buf, t, cap);
}

/* ---- draw --------------------------------------------------------------- */
static void settings_draw(window_t *w, int cx, int cy, int cw, int ch) {
    w->accent = COL_ACCENT;                 /* the Settings window tracks the theme */
    g_fill(cx, cy, cw, ch, COL_PANEL);
    nhot = 0; org_x = cx; org_y = cy;

    int pad = 16, x0 = cx + pad, right = cx + cw - pad, y = cy + pad;

    y = section(x0, y, "Theme");
    y = chip_row(x0, right, y, K_THEME, settings_theme_count(),
                 settings_theme_name, g_settings.theme);

    y = section(x0, y, "Accent");
    y = swatch_row(x0, right, y, K_ACCENT, settings_accent_count(),
                   settings_accent_color, g_settings.accent);

    y = section(x0, y, "Wallpaper");
    y = chip_row(x0, right, y, K_WALLSTYLE, WALL_COUNT,
                 settings_wallstyle_name, g_settings.wall_style);
    y = swatch_row(x0, right, y, K_WALLCOLOR, settings_wallcolor_count(),
                   settings_wallcolor, g_settings.wall_color);

    y = section(x0, y, "Font");
    y = chip_row(x0, right, y, K_FONT, settings_font_count(),
                 settings_font_name, g_settings.font);

    y = section(x0, y, "Resolution");
    y = chip_row(x0, right, y, K_RES, settings_res_count(),
                 settings_res_name, g_settings.res_index);

    y = section(x0, y, "Audio device");
    y = chip_row(x0, right, y, K_AUDIO, settings_audio_count(),
                 settings_audio_name, g_settings.audio_device);
    {   /* test-tone button */
        int tw = g_text_width("Test", 1) + 24;
        chip(x0, y, "Test", 0);
        hot_add(x0, y, tw, 30, K_AUDIOTEST, 0);
        y += 30 + 16;
    }

    /* info footer */
    int fy = cy + ch - 36;
    g_hline(x0, fy - 8, cw - 2 * pad, COL_PANEL_3);
    char line[64], d[24];
    fmt_dims(d, sizeof(d), gui_panel_w(), gui_panel_h());
    line[0] = 0; kstrlcat(line, "Panel    ", sizeof(line)); kstrlcat(line, d, sizeof(line));
    g_text(x0, fy, line, COL_TEXT_DIM, 1);
    line[0] = 0; kstrlcat(line, "Audio    ", sizeof(line));
    kstrlcat(line, settings_audio_name(g_settings.audio_device), sizeof(line));
    g_text(x0 + 210, fy, line, COL_TEXT_DIM, 1);
}

/* ---- input -------------------------------------------------------------- */
static void apply_hot(int kind, int val) {
    switch (kind) {
    case K_THEME:     g_settings.theme = val;
                      g_settings.accent = settings_theme_accent(val); break;
    case K_ACCENT:    g_settings.accent = settings_accent_color(val); break;
    case K_WALLSTYLE: g_settings.wall_style = val; gui_clear_wallpaper_image(); break;
    case K_WALLCOLOR: g_settings.wall_color = settings_wallcolor(val); gui_clear_wallpaper_image(); break;
    case K_RES:       g_settings.res_index = val; break;
    case K_AUDIO:     g_settings.audio_device = val; break;
    case K_FONT:      g_settings.font = val; break;
    case K_AUDIOTEST:
        /* play 880 Hz for ~200 ms; the window tick silences it. Honours mute
         * (pcspk_tone is a no-op when the device is set to Muted). */
        pcspk_tone(880);
        beep_off_at = pit_ticks() + 200;
        return;                          /* no settings_apply needed */
    default: return;
    }
    settings_apply();
}

static void settings_tick(window_t *w) {
    (void)w;
    if (beep_off_at && pit_ticks() >= beep_off_at) { pcspk_off(); beep_off_at = 0; }
}

static void settings_click(window_t *w, int lx, int ly) {
    (void)w;
    for (int i = 0; i < nhot; i++) {
        hot_t *h = &hots[i];
        if (lx < h->x || lx >= h->x + h->w || ly < h->y || ly >= h->y + h->h) continue;
        apply_hot(h->kind, h->val);
        return;
    }
}

void settings_app_init(void) {
    window_t *win = gui_add_window("Settings", 580, 596, COL_ACCENT, ICON_SETTINGS);
    if (!win) return;
    win->draw  = settings_draw;
    win->click = settings_click;
    win->tick  = settings_tick;
    win->min_w = 440; win->min_h = 340;
    win->x = 150; win->y = 80;
}
