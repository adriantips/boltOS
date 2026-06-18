#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltOS  -  include/settings.h
 *  Desktop settings model: theme preset, accent, wallpaper, virtual
 *  resolution and aspect ratio. The Settings app edits g_settings then calls
 *  settings_apply(), which retints the live theme (g_theme) and asks the GUI
 *  to relayout the logical desktop / letterbox and re-render the wallpaper.
 * ===========================================================================*/

/* wallpaper rendering styles */
enum { WALL_GRADIENT = 0, WALL_SOLID, WALL_GLOW, WALL_GRID, WALL_COUNT };

typedef struct {
    int      theme;         /* preset index   (settings_theme_*)        */
    uint32_t accent;        /* accent colour override                   */
    int      wall_style;    /* WALL_*                                   */
    uint32_t wall_color;    /* wallpaper base colour                    */
    int      res_index;     /* resolution table index (real DISPI mode)  */
    int      aspect_index;  /* unused, kept for ABI                      */
    int      audio_device;  /* 0 = PC Speaker, 1 = Muted                 */
    int      font;          /* 0 = Retro 8x8, 1 = Arial 8x16 (default)   */
} settings_t;

extern settings_t g_settings;

void settings_init(void);     /* defaults; call once after the panel size known */
void settings_apply(void);    /* push g_settings -> theme + display             */

/* ---- option tables (data lives in settings.c; the UI walks these) ------- */
int         settings_theme_count(void);
const char *settings_theme_name(int i);
uint32_t    settings_theme_accent(int i);          /* preset's default accent  */
uint32_t    settings_theme_swatch(int i);          /* representative chip color */

int         settings_accent_count(void);
uint32_t    settings_accent_color(int i);

int         settings_wallcolor_count(void);
uint32_t    settings_wallcolor(int i);

const char *settings_wallstyle_name(int i);        /* i in 0..WALL_COUNT-1     */

int         settings_res_count(void);
const char *settings_res_name(int i);
void        settings_res_dims(int i, int *w, int *h);   /* native/oversize -> panel */

int         settings_aspect_count(void);
const char *settings_aspect_name(int i);
void        settings_aspect_ratio(int i, int *num, int *den);  /* auto -> 0,0 */

int         settings_audio_count(void);
const char *settings_audio_name(int i);

int         settings_font_count(void);
const char *settings_font_name(int i);
