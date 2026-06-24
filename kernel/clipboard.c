/* ===========================================================================
 *  BoltOS  -  kernel/clipboard.c
 *  System-wide text clipboard shared by every GUI app. Copy in one window,
 *  paste in another. Backed by one static buffer (no allocation, no failure
 *  paths) capped at CLIP_CAP bytes; longer copies are truncated.
 * ===========================================================================*/
#include "clipboard.h"

static char g_clip[CLIP_CAP];
static int  g_len;

void clip_set(const char *s, int len) {
    if (!s || len <= 0) { g_len = 0; g_clip[0] = 0; return; }
    if (len > CLIP_CAP - 1) len = CLIP_CAP - 1;
    for (int i = 0; i < len; i++) g_clip[i] = s[i];
    g_clip[len] = 0;
    g_len = len;
}

const char *clip_get(void) { return g_clip; }
int         clip_len(void) { return g_len; }
