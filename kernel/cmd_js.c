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
#include "http.h"
#include "proc.h"

static void cli_log(void *ud, const char *m) { (void)ud; kprintf("%s\n", m); }
static js_dom_node cli_byid(void *ud, const char *id) { (void)ud; (void)id; return 0; }
static js_dom_node cli_bytag(void *ud, const char *t, int i) { (void)ud; (void)t; (void)i; return 0; }
static void cli_setinner(void *ud, js_dom_node n, const char *h) { (void)ud; (void)n; (void)h; }
static int  cli_getinner(void *ud, js_dom_node n, char *o, uint32_t c) { (void)ud; (void)n; if(c)o[0]=0; return 0; }
static void cli_settext(void *ud, js_dom_node n, const char *t) { (void)ud; (void)n; (void)t; }
static void cli_write(void *ud, const char *h) { (void)ud; kprintf("%s", h); }
static void cli_title(void *ud, const char *t) { (void)ud; (void)t; }
/* fetch(): one-shot HTTP GET so shell scripts can do fetch().then(...) too */
static int cli_fetch(void *ud, const char *url, char *out, uint32_t cap, int *status) {
    (void)ud; if(cap)out[0]=0; int code=0; char loc[256];
    int n = http_get(url, out, cap, &code, loc, sizeof loc);
    if(status)*status=code;
    return n;
}

static js_host cli_host = {
    0, cli_byid, cli_bytag, cli_setinner, cli_getinner, cli_settext, cli_write, cli_title, cli_log,
    0, 0, 0, 0, 0,    /* query/createElement/append/set+get attr: no DOM in the shell */
    0, 0, 0,          /* localStorage */
    0, 0,             /* document.cookie */
    cli_fetch         /* fetch() */
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
    /* A script file runs in RING 3: exec /bin/js (the same BoltJS engine built
     * as a userland ELF) with the path as argv[1]. The interpreter executes in
     * ring 3, reading the file and printing output through syscalls -- no part
     * of the language runtime touches kernel mode. Output goes to the console. */
    fs_node *f = fs_lookup(argv[1]);
    if (!f)        { kprintf("js: can't open '%s'\n", argv[1]); return 1; }
    if (f->is_dir) { kprintf("js: '%s' is a directory\n", argv[1]); return 1; }
    if (proc_exec_argv("/bin/js", argv[1]) < 0) {
        kprintf("js: could not start ring-3 interpreter\n");
        return 1;
    }
    return 0;
}

/* `webx [url-or-path]` -- launch the RING-3 web browser (/bin/browser). The
 * whole rendering pipeline (HTML->DOM, CSS cascade + box/flex/grid layout, and
 * JavaScript) executes in ring 3; it grabs the framebuffer via SYS_FBINFO,
 * reads keys via SYS_GETKEY and fetches over the net via SYS_HTTPGET. Press q
 * inside the browser to return to the desktop. */
int cmd_webx(int argc, char **argv) {
    const char *url = (argc > 1) ? argv[1] : "/web/index.html";
    if (proc_exec_argv("/bin/browser", url) < 0) {
        kprintf("webx: could not start ring-3 browser\n");
        return 1;
    }
    kprintf("webx: ring-3 browser launched (%s); press q to quit\n", url);
    return 0;
}
