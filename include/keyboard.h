#pragma once
/* PS/2 keyboard driver. The IRQ1 handler decodes scancodes (with shift) into
 * ASCII and pushes them onto a ring buffer; the shell drains it below. */
/* Extended keys are delivered as otherwise-unused control bytes so the existing
 * "c >= 32 is printable" callers ignore them while editors can act on them. */
#define KEY_UP    0x11
#define KEY_DOWN  0x12
#define KEY_LEFT  0x13
#define KEY_RIGHT 0x14
#define KEY_HOME  0x02
#define KEY_END   0x05
#define KEY_PGUP  0x10
#define KEY_PGDN  0x0E
#define KEY_DEL   0x7F
#define KEY_SHOT  0x17   /* F12: capture a screenshot (handled globally by GUI) */
#define KEY_ALTTAB  0x0B /* Alt+Tab: cycle to next window (handled by GUI)      */
#define KEY_ALTTABR 0x0C /* Shift+Alt+Tab: cycle to previous window             */
#define KEY_ALTF4   0x04 /* Alt+F4: close the focused window (handled by GUI)   */
#define KEY_WINTAP  0x07 /* Super/Win tapped alone: toggle Start menu (GUI)     */

/* Clipboard / editing shortcuts. Emitted by the driver when Ctrl is held with
 * the matching letter. The values are the classic ASCII control codes and were
 * chosen to not collide with any KEY_* above. */
#define KEY_SELALL 0x01   /* Ctrl+A */
#define KEY_COPY   0x03   /* Ctrl+C */
#define KEY_PASTE  0x16   /* Ctrl+V */
#define KEY_CUT    0x18   /* Ctrl+X */
#define KEY_SAVE   0x19   /* Ctrl+S */

/* Shift+navigation keys, for extending a text selection. Distinct control bytes
 * so editors can tell "move caret" from "extend selection". */
#define KEY_SLEFT  0x1C
#define KEY_SRIGHT 0x1D
#define KEY_SUP    0x1E
#define KEY_SDOWN  0x1F
#define KEY_SHOME  0x06
#define KEY_SEND   0x15

void keyboard_init(void);
int  kbd_ctrl_down(void);  /* 1 while either Ctrl key is held */
int  kbd_shift_down(void); /* 1 while either Shift key is held */
int  kbd_win_down(void);   /* 1 while either Super/Win key is held */
void kbd_inject(char c);   /* push a decoded char into the ring (USB HID path) */
int  kbd_trygetc(void);   /* next char, or -1 if the buffer is empty (non-blocking) */
char kbd_getc(void);      /* block (hlt) until a key is available, then return it */

/* Raw scancode tap for clients needing real key up/down (DOOM). When enabled,
 * every make/break is queued; kbd_raw_get() returns the next event or -1.
 * Encoding: bits0-6 scancode, 0x100 = release, 0x200 = extended (0xE0). */
void kbd_raw_enable(int on);
int  kbd_raw_get(void);
