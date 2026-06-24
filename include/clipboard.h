#pragma once
#include <stdint.h>
/* ===========================================================================
 *  BoltOS system clipboard. A single shared text buffer that any GUI app can
 *  read and write, so Ctrl+C / Ctrl+X / Ctrl+V move text between apps. Plain
 *  UTF-8/ASCII bytes; the store is NUL-terminated for convenience.
 * ===========================================================================*/
#define CLIP_CAP 16384

void        clip_set(const char *s, int len);   /* copy len bytes into the clipboard */
const char *clip_get(void);                     /* NUL-terminated current contents    */
int         clip_len(void);                     /* byte length (0 == empty)           */
