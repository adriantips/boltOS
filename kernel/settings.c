/* ===========================================================================
 *  BoltOS  -  kernel/settings.c
 *  The live desktop theme (g_theme) and the editable settings model
 *  (g_settings), plus the option tables the Settings app renders. Applying a
 *  change retints the theme and hands off to the GUI to relayout + repaint.
 * ===========================================================================*/
#include <stdint.h>
#include "settings.h"
#include "gui.h"
#include "pcspk.h"

/* Live palette. Starts as the Midnight preset so any early COL_* read (before
 * settings_init) still produces a sane dark desktop. */
theme_t g_theme = {
    .accent = 0x2D7FF9, .accent_dim = 0x1E5BB8,
    .text   = 0xECECF0, .text_dim   = 0x9CA3AF,
    .panel  = 0x1E1E27, .panel2     = 0x282833, .panel3 = 0x32323F,
    .good   = 0x46D17B, .warn       = 0xF2C14E, .bad    = 0xE8506B,
};

settings_t g_settings;

/* ---- theme presets ------------------------------------------------------ */
static const theme_t presets[] = {
    /* Midnight (default dark) */
    { 0x2D7FF9, 0x1E5BB8, 0xECECF0, 0x9CA3AF, 0x1E1E27, 0x282833, 0x32323F,
      0x46D17B, 0xF2C14E, 0xE8506B },
    /* Light */
    { 0x2D7FF9, 0x9CC3FB, 0x1B1D24, 0x5C6270, 0xF3F4F8, 0xE5E7EF, 0xD3D6E1,
      0x1E9E5A, 0xC98A12, 0xD23A57 },
    /* Slate (cool grey-blue dark) */
    { 0x4C8DFF, 0x2E5699, 0xE6E8EE, 0x8A93A6, 0x191B22, 0x232733, 0x2F3543,
      0x4FD18A, 0xE9B84B, 0xE85D74 },
    /* Forest (green dark) */
    { 0x3DBB6E, 0x247046, 0xE8F0EA, 0x90A89A, 0x14201A, 0x1C2C24, 0x26392F,
      0x6FE0A0, 0xE6C24A, 0xE06A6A },
    /* Rose (magenta dark) */
    { 0xE05A8A, 0x8C2F52, 0xF1E8EE, 0xB793A6, 0x1F1822, 0x2C2230, 0x392B3E,
      0x5FD0C0, 0xF0B24A, 0xF06A8A },
};
static const char *preset_names[] = { "Midnight", "Light", "Slate", "Forest", "Rose" };
#define NPRESET ((int)(sizeof(presets) / sizeof(presets[0])))

/* ---- accent palette ----------------------------------------------------- */
static const uint32_t accents[] = {
    0x2D7FF9, 0x4C8DFF, 0x9B6CF2, 0xE05A8A, 0xE8506B,
    0xF2884E, 0xF2C14E, 0x46D17B, 0x2BC4C4, 0xB0B6C2,
};
#define NACCENT ((int)(sizeof(accents) / sizeof(accents[0])))

/* ---- wallpaper base colours --------------------------------------------- */
static const uint32_t wallcolors[] = {
    0x0A1733, 0x101626, 0x1A2238, 0x12231C, 0x231423,
    0x2A1A12, 0x14181C, 0x202024, 0x0C1414, 0x000000,
};
#define NWALLC ((int)(sizeof(wallcolors) / sizeof(wallcolors[0])))

static const char *wallstyle_names[] = { "Gradient", "Solid", "Glow", "Grid" };

/* ---- resolution table (REAL physical panel size via DISPI) -------------- *
 * These reprogram the GPU to a true native mode -- the framebuffer becomes the
 * picked size with no upscaling and no letterbox bars, so the picture stays
 * crisp at every step. 1024x768 is the boot default. */
static const struct { const char *name; int w, h; } res_tab[] = {
    { "1024x768", 1024, 768 },
    { "720p",     1280, 720  },
    { "1080p",    1920, 1080 },
    { "1440p",    2560, 1440 },
    { "4K",       3840, 2160 },
};
#define NRES ((int)(sizeof(res_tab) / sizeof(res_tab[0])))

/* ---- audio output device ------------------------------------------------ */
static const char *audio_names[] = { "PC Speaker", "Muted" };
#define NAUDIO ((int)(sizeof(audio_names) / sizeof(audio_names[0])))

/* ---- aspect table (output rectangle shape) ------------------------------ */
static const struct { const char *name; int num, den; } asp_tab[] = {
    { "Auto", 0, 0 },          /* 0,0 -> preserve logical aspect */
    { "16:9",  16, 9 },
    { "16:10", 16, 10 },
    { "4:3",   4,  3 },
    { "1:1",   1,  1 },
    { "21:9",  21, 9 },
};
#define NASP ((int)(sizeof(asp_tab) / sizeof(asp_tab[0])))

/* ---- table accessors ---------------------------------------------------- */
int         settings_theme_count(void)       { return NPRESET; }
const char *settings_theme_name(int i)       { return (i >= 0 && i < NPRESET) ? preset_names[i] : ""; }
uint32_t    settings_theme_accent(int i)     { return (i >= 0 && i < NPRESET) ? presets[i].accent : 0x2D7FF9; }
uint32_t    settings_theme_swatch(int i)     { return (i >= 0 && i < NPRESET) ? presets[i].panel2 : 0x282833; }

int         settings_accent_count(void)      { return NACCENT; }
uint32_t    settings_accent_color(int i)     { return (i >= 0 && i < NACCENT) ? accents[i] : 0x2D7FF9; }

int         settings_wallcolor_count(void)   { return NWALLC; }
uint32_t    settings_wallcolor(int i)        { return (i >= 0 && i < NWALLC) ? wallcolors[i] : 0x0A1733; }

const char *settings_wallstyle_name(int i)   { return (i >= 0 && i < WALL_COUNT) ? wallstyle_names[i] : ""; }

int         settings_res_count(void)         { return NRES; }
const char *settings_res_name(int i)         { return (i >= 0 && i < NRES) ? res_tab[i].name : ""; }
void settings_res_dims(int i, int *w, int *h) {
    int rw = (i >= 0 && i < NRES) ? res_tab[i].w : 1024;
    int rh = (i >= 0 && i < NRES) ? res_tab[i].h : 768;
    if (w) *w = rw;
    if (h) *h = rh;
}

int         settings_audio_count(void)       { return NAUDIO; }
const char *settings_audio_name(int i)       { return (i >= 0 && i < NAUDIO) ? audio_names[i] : ""; }

int         settings_aspect_count(void)      { return NASP; }
const char *settings_aspect_name(int i)      { return (i >= 0 && i < NASP) ? asp_tab[i].name : ""; }
void settings_aspect_ratio(int i, int *num, int *den) {
    if (num) *num = (i >= 0 && i < NASP) ? asp_tab[i].num : 0;
    if (den) *den = (i >= 0 && i < NASP) ? asp_tab[i].den : 0;
}

/* ---- apply -------------------------------------------------------------- */
/* scale each RGB channel of c by n/255 */
static uint32_t shade(uint32_t c, int n) {
    int r = ((c >> 16 & 0xFF) * n) / 255;
    int g = ((c >> 8  & 0xFF) * n) / 255;
    int b = ((c       & 0xFF) * n) / 255;
    return (uint32_t)(r << 16 | g << 8 | b);
}

void settings_init(void) {
    g_settings.theme        = 0;
    g_settings.accent       = settings_theme_accent(0);
    g_settings.wall_style   = WALL_GRADIENT;
    g_settings.wall_color   = 0x0A1733;
    g_settings.res_index    = 0;     /* 1024x768 boot default */
    g_settings.aspect_index = 0;     /* unused (kept for ABI) */
    g_settings.audio_device = 0;     /* PC Speaker */
    settings_apply();
}

void settings_apply(void) {
    int t = g_settings.theme;
    if (t < 0 || t >= NPRESET) t = 0;
    g_theme = presets[t];
    /* user accent overrides the preset's; derive a dim variant for it */
    g_theme.accent     = g_settings.accent;
    g_theme.accent_dim = shade(g_settings.accent, 158);
    pcspk_set_enabled(g_settings.audio_device == 0);   /* PC Speaker vs Muted */
    gui_apply_display();             /* re-mode the panel + repaint if changed */
}
