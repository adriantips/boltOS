#include <stdint.h>
#include "interrupts.h"
#include "io.h"
#include "keyboard.h"

/* extended (0xE0-prefixed) make code -> our control-byte key code, or 0 */
static char ext_map(uint8_t sc) {
    switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x53: return KEY_DEL;
        default:   return 0;
    }
}

/* US scancode set 1 -> ASCII. Two tables: unshifted and shifted. Indices match
 * the make-code of each key; release codes (bit 7 set) are handled separately. */
static const char map[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const char shiftmap[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0,'|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ',
};

/* Single-producer (IRQ) / single-consumer (shell) ring. 8-bit indices wrap at
 * 256 == RBSZ, so the modulo is free. */
#define RBSZ 256
static volatile char    rb[RBSZ];
static volatile uint8_t rhead, rtail;
static volatile int     shift;
static volatile int     extended;        /* last byte was the 0xE0 prefix */

static void push(char c) {
    uint8_t n = (uint8_t)(rtail + 1);
    if (n != rhead) { rb[rtail] = c; rtail = n; }   /* drop on overflow */
}

static void on_key(struct registers *r) {
    (void)r;
    uint8_t sc = inb(0x60);
    if (sc == 0xE0) { extended = 1; return; }       /* prefix: next byte is extended */
    if (extended) {
        extended = 0;
        if (!(sc & 0x80)) { char e = ext_map(sc); if (e) push(e); }
        return;                                     /* swallow the release too */
    }
    switch (sc) {
        case 0x2A: case 0x36: shift = 1; return;    /* L/R shift pressed  */
        case 0xAA: case 0xB6: shift = 0; return;    /* L/R shift released */
    }
    if (sc & 0x80) return;                          /* any other key release */
    char c = shift ? shiftmap[sc] : map[sc];
    if (c) push(c);
}

int kbd_trygetc(void) {
    if (rhead == rtail) return -1;
    char c = rb[rhead];
    rhead = (uint8_t)(rhead + 1);
    return (int)(unsigned char)c;
}

char kbd_getc(void) {
    int c;
    while ((c = kbd_trygetc()) < 0) __asm__ volatile("hlt");
    return (char)c;
}

void keyboard_init(void) {
    rhead = rtail = 0;
    shift = 0;
    extended = 0;
    irq_install(1, on_key);
    inb(0x60);                                      /* drain any pending byte */
}
