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

void keyboard_init(void);
int  kbd_trygetc(void);   /* next char, or -1 if the buffer is empty (non-blocking) */
char kbd_getc(void);      /* block (hlt) until a key is available, then return it */
