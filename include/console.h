#pragma once
/* On-screen text console over the framebuffer. Maintains a character grid so it
 * can scroll, erase (backspace), clear, and blink a cursor without ever reading
 * back from VRAM. kputc() (see kprintf.h) is the output sink. Safe to use with
 * no framebuffer (everything becomes serial-only). */
void console_init(void);
void console_clear(void);        /* wipe the grid + screen, home the cursor */
void console_cursor_tick(void);  /* call from an idle loop to blink the cursor */
void console_set_sink(void (*fn)(char));  /* redirect kputc (GUI terminal capture); 0 = restore */
void (*console_get_sink(void))(char);     /* current sink, so callers can save/restore it */
void console_detach(void);       /* stop drawing the text console to the framebuffer */
