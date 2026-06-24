/* ===========================================================================
 *  ob_hotlist.c  --  NetSurf desktop/hotlist.c (bookmarks)
 *
 *  The hotlist: a flat list of bookmarked (url, title) pairs, loaded from and
 *  written back to the BoltOS filesystem so bookmarks survive reboots. NetSurf
 *  stores a richer tree; we keep the same public surface (add / remove / has /
 *  enumerate) at a flat granularity, which is all the framebuffer toolbar needs.
 * ===========================================================================*/
#include "oldbrowser.h"
#include "string.h"
#include "fs.h"

#define HOT_MAX   64
#define HOT_PATH  "/oldbrowser/hotlist.txt"

typedef struct { char url[256]; char title[120]; } hotentry;
static hotentry hot[HOT_MAX];
static int      nhot;
static int      loaded;

static void scopy(char *d, const char *s, uint32_t cap) {
    if (!cap) return; uint32_t i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}

static void hotlist_save(void) {
    char buf[HOT_MAX * 384]; uint32_t o = 0;
    for (int i = 0; i < nhot && o < sizeof(buf) - 384; i++) {
        const char *u = hot[i].url, *t = hot[i].title;
        while (*u) buf[o++] = *u++;
        buf[o++] = '\t';
        while (*t) buf[o++] = *t++;
        buf[o++] = '\n';
    }
    buf[o] = 0;
    fs_lookup("/oldbrowser") ? (void)0 : (void)fs_create("/oldbrowser", 1);
    fs_node *n = fs_lookup(HOT_PATH);
    if (!n) n = fs_create(HOT_PATH, 0);
    if (n) fs_write(n, buf, o);
}

void hotlist_init(void) {
    if (loaded) return;
    loaded = 1; nhot = 0;
    fs_node *n = fs_lookup(HOT_PATH);
    if (!n || n->is_dir || !n->data) return;
    const char *p = (const char *)n->data;
    const char *end = p + n->size;
    while (p < end && nhot < HOT_MAX) {
        const char *tab = p; while (tab < end && *tab != '\t' && *tab != '\n') tab++;
        if (tab >= end || *tab != '\t') { while (p < end && *p != '\n') p++; if (p < end) p++; continue; }
        uint32_t ul = (uint32_t)(tab - p); if (ul > 255) ul = 255;
        memcpy(hot[nhot].url, p, ul); hot[nhot].url[ul] = 0;
        const char *nl = tab + 1; while (nl < end && *nl != '\n') nl++;
        uint32_t tl = (uint32_t)(nl - (tab + 1)); if (tl > 119) tl = 119;
        memcpy(hot[nhot].title, tab + 1, tl); hot[nhot].title[tl] = 0;
        nhot++;
        p = (nl < end) ? nl + 1 : end;
    }
}

int hotlist_has(const char *url) {
    for (int i = 0; i < nhot; i++) if (strcmp(hot[i].url, url) == 0) return 1;
    return 0;
}

int hotlist_add(const char *url, const char *title) {
    if (!url || !url[0] || nhot >= HOT_MAX) return -1;
    if (hotlist_has(url)) return 0;
    scopy(hot[nhot].url, url, sizeof(hot[nhot].url));
    scopy(hot[nhot].title, title && title[0] ? title : url, sizeof(hot[nhot].title));
    nhot++;
    hotlist_save();
    return 0;
}

int hotlist_remove(const char *url) {
    for (int i = 0; i < nhot; i++) if (strcmp(hot[i].url, url) == 0) {
        for (int j = i + 1; j < nhot; j++) hot[j-1] = hot[j];
        nhot--; hotlist_save(); return 0;
    }
    return -1;
}

int hotlist_count(void) { return nhot; }

int hotlist_get(int i, char *url, uint32_t ucap, char *title, uint32_t tcap) {
    if (i < 0 || i >= nhot) return -1;
    if (url) scopy(url, hot[i].url, ucap);
    if (title) scopy(title, hot[i].title, tcap);
    return 0;
}
