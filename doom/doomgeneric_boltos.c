/* ===========================================================================
 *  doomgeneric_boltos.c  -  the BoltOS platform layer for doomgeneric.
 *
 *  doomgeneric abstracts a port down to a handful of DG_* callbacks plus a
 *  Create/Tick pair. Here they are wired to the BoltOS kernel:
 *    - DG_ScreenBuffer (640x400 ARGB) is painted to a GUI window by app_doom.c
 *    - timing comes from the 1000 Hz PIT
 *    - keys arrive as raw PS/2 scancodes (real make/break) and are translated
 *      to DOOM key codes here
 *    - fatal errors (I_Error/exit) longjmp back to the app instead of killing
 *      the kernel
 *
 *  The IWAD is an embedded blob; dg_fs_register() exposes it to the libc shim
 *  as the file "doom1.wad" before D_DoomMain opens it.
 * ===========================================================================*/
#include <stdint.h>
#include <stddef.h>

#include "doomkeys.h"
#include "doomgeneric.h"

#include "pit.h"

/* embedded shareware IWAD (objcopy blob, see build.sh) */
extern const uint8_t _binary_doom1_wad_start[];
extern const uint8_t _binary_doom1_wad_end[];

/* libc-shim file registration */
void dg_fs_register(const char *name, const uint8_t *data, uint32_t size);

/* doomgeneric entry points */
void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

/* ---- fatal-error trampoline ------------------------------------------------
 *  DOOM calls exit()/abort() on fatal errors; dg_libc routes those to
 *  dg_panic(), which unwinds back to whichever bridge call was running. */
static void *g_panic_buf[5];
static volatile int g_status = 0;          /* 0 = idle, 1 = running, 2 = failed */

void dg_panic(void) {
    __builtin_longjmp(g_panic_buf, 1);
}

int dg_boltos_status(void) { return g_status; }

/* ---- key queue (DOOM-side) ------------------------------------------------*/
#define KEYQ 32
static uint16_t s_keyq[KEYQ];
static volatile unsigned s_kw = 0, s_kr = 0;

static void queue_key(int pressed, unsigned char key) {
    if (!key) return;
    s_keyq[s_kw % KEYQ] = (uint16_t)((pressed << 8) | key);
    s_kw++;
}

/* US scancode set 1 -> ASCII (unshifted), for menu text / cheats */
static const unsigned char sc_ascii[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x7f, 9,
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13,  0,   'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`', 0,   '\\','z', 'x', 'c',  'v',
    'b',  'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ',
};

/* Translate a PS/2 scancode (make code, 0..0x7f) to a DOOM key code.
 * `ext` marks the 0xE0-prefixed variants (arrows, right ctrl/alt, keypad). */
static unsigned char to_doomkey(int sc, int ext) {
    if (ext) {
        switch (sc) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1D: return KEY_FIRE;        /* right ctrl */
        case 0x38: return KEY_RALT;        /* right alt  */
        case 0x1C: return KEY_ENTER;       /* keypad enter */
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INS;
        case 0x53: return KEY_DEL;
        default:   return 0;
        }
    }
    switch (sc) {
    case 0x01: return KEY_ESCAPE;
    case 0x1C: return KEY_ENTER;
    case 0x0F: return KEY_TAB;
    case 0x0E: return KEY_BACKSPACE;
    case 0x39: return KEY_USE;             /* space */
    case 0x1D: return KEY_FIRE;            /* left ctrl */
    case 0x2A: case 0x36: return KEY_RSHIFT;
    case 0x38: return KEY_RALT;            /* left alt = strafe */
    case 0x3B: return KEY_F1;
    case 0x3C: return KEY_F2;
    case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4;
    case 0x3F: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    case 0x0C: return KEY_MINUS;
    case 0x0D: return KEY_EQUALS;
    default:   return (sc < 128) ? sc_ascii[sc] : 0;
    }
}

/* Called by app_doom for every raw scancode event. */
void dg_boltos_key(int pressed, int scancode, int ext) {
    queue_key(pressed, to_doomkey(scancode, ext));
}

/* ===========================================================================
 *  bridge: create + tick, guarded against fatal DOOM errors
 * ===========================================================================*/
static char *dg_argv[] = { (char *)"doom", (char *)"-iwad", (char *)"doom1.wad",
                           (char *)"-warp", (char *)"1", (char *)"1", NULL };
#define DG_ARGC 6

void dg_boltos_create(void) {
    if (__builtin_setjmp(g_panic_buf)) { g_status = 2; return; }
    dg_fs_register("doom1.wad", _binary_doom1_wad_start,
                   (uint32_t)(_binary_doom1_wad_end - _binary_doom1_wad_start));
    doomgeneric_Create(DG_ARGC, dg_argv);
    g_status = 1;
}

void dg_boltos_tick(void) {
    if (g_status != 1) return;
    if (__builtin_setjmp(g_panic_buf)) { g_status = 2; return; }
    doomgeneric_Tick();
}

/* ===========================================================================
 *  DG_* platform callbacks
 * ===========================================================================*/
void DG_Init(void) { }

void DG_DrawFrame(void) { }     /* app_doom.c blits DG_ScreenBuffer each frame */

void DG_SleepMs(uint32_t ms) { (void)ms; }   /* cooperative; the GUI loop paces us */

uint32_t DG_GetTicksMs(void) { return (uint32_t)pit_ticks(); }   /* PIT is 1000 Hz */

int DG_GetKey(int *pressed, unsigned char *key) {
    if (s_kr == s_kw) return 0;
    uint16_t v = s_keyq[s_kr % KEYQ];
    s_kr++;
    *pressed = v >> 8;
    *key = (unsigned char)(v & 0xFF);
    return 1;
}

static char s_title[64];
void DG_SetWindowTitle(const char *title) {
    int i = 0; if (title) { while (title[i] && i < 63) { s_title[i] = title[i]; i++; } } s_title[i] = 0;
}
const char *dg_boltos_title(void) { return s_title[0] ? s_title : "DOOM"; }
