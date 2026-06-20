/* ===========================================================================
 *  BoltOS  -  kernel/cmd_js.c
 *  The `js` shell command: run JavaScript through the BoltJS engine.
 *    js -c "code..."   run inline code
 *    js FILE.js        run a script from the ramfs
 *  console.log() and friends print to the terminal; there is no DOM here, so
 *  document.* calls are inert (the browser supplies a real DOM host).
 * ===========================================================================*/
#include <stdint.h>
#include "commands.h"
#include "js.h"
#include "kprintf.h"
#include "string.h"
#include "kheap.h"
#include "fs.h"

static void cli_log(void *ud, const char *m) { (void)ud; kprintf("%s\n", m); }
static js_dom_node cli_byid(void *ud, const char *id) { (void)ud; (void)id; return 0; }
static js_dom_node cli_bytag(void *ud, const char *t, int i) { (void)ud; (void)t; (void)i; return 0; }
static void cli_setinner(void *ud, js_dom_node n, const char *h) { (void)ud; (void)n; (void)h; }
static int  cli_getinner(void *ud, js_dom_node n, char *o, uint32_t c) { (void)ud; (void)n; if(c)o[0]=0; return 0; }
static void cli_settext(void *ud, js_dom_node n, const char *t) { (void)ud; (void)n; (void)t; }
static void cli_write(void *ud, const char *h) { (void)ud; kprintf("%s", h); }
static void cli_title(void *ud, const char *t) { (void)ud; (void)t; }

static js_host cli_host = {
    0, cli_byid, cli_bytag, cli_setinner, cli_getinner, cli_settext, cli_write, cli_title, cli_log
};

static int run_src(const char *src, uint32_t len) {
    char err[160];
    int rc = js_run(src, len, &cli_host, err, sizeof err);
    if (rc) kprintf("js: %s\n", err);
    return rc ? 1 : 0;
}

int cmd_js(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: js -c \"code\"   |   js FILE.js\n");
        return 1;
    }
    if (strcmp(argv[1], "-c") == 0) {
        if (argc < 3) { kprintf("js: -c needs code\n"); return 1; }
        char code[4096]; int n = 0;
        for (int i = 2; i < argc && n < (int)sizeof(code) - 2; i++) {
            if (i > 2) code[n++] = ' ';
            for (int j = 0; argv[i][j] && n < (int)sizeof(code) - 2; j++) code[n++] = argv[i][j];
        }
        code[n] = 0;
        return run_src(code, (uint32_t)n);
    }
    fs_node *f = fs_lookup(argv[1]);
    if (!f)        { kprintf("js: can't open '%s'\n", argv[1]); return 1; }
    if (f->is_dir) { kprintf("js: '%s' is a directory\n", argv[1]); return 1; }
    char *src = (char *)kmalloc(f->size + 1);
    if (!src) { kprintf("js: out of memory\n"); return 1; }
    for (uint32_t i = 0; i < f->size; i++) src[i] = (char)f->data[i];
    src[f->size] = 0;
    int rc = run_src(src, f->size);
    kfree(src);
    return rc;
}
