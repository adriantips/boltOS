/* ===========================================================================
 *  BoltOS  -  user/js_main.c
 *  Ring-3 BoltJS interpreter. Same engine as kernel/js.c, but compiled as a
 *  userland ELF and run in ring 3: it reads a script file through the SYS_OPEN/
 *  SYS_READ/SYS_FSTAT syscalls and prints console.log()/document.write() output
 *  through SYS_WRITE. No part of the language runtime executes in kernel mode.
 *
 *      usage:  js <file.js>
 *
 *  There is no DOM in this standalone host (document.* is inert) and fetch() is
 *  unavailable until ring-3 networking syscalls land; both degrade gracefully.
 * ===========================================================================*/
#include "ulibc.h"
#include "js.h"

static void       h_log  (void *u, const char *m)                 { (void)u; printf("%s\n", m); }
static void       h_write(void *u, const char *h)                 { (void)u; printf("%s", h); }
static js_dom_node h_byid (void *u, const char *i)                { (void)u; (void)i; return 0; }
static js_dom_node h_bytag(void *u, const char *t, int i)         { (void)u; (void)t; (void)i; return 0; }
static void       h_sinner(void *u, js_dom_node n, const char *h){ (void)u; (void)n; (void)h; }
static int        h_ginner(void *u, js_dom_node n, char *o, uint32_t c){ (void)u; (void)n; if (c) o[0] = 0; return 0; }
static void       h_stext (void *u, js_dom_node n, const char *t){ (void)u; (void)n; (void)t; }
static void       h_title (void *u, const char *t)               { (void)u; (void)t; }

/* Field order matches struct js_host in include/js.h. DOM/storage/cookie/fetch
 * callbacks are null: a CLI script that never touches them runs unaffected. */
static js_host host = {
    0, h_byid, h_bytag, h_sinner, h_ginner, h_stext, h_write, h_title, h_log,
    0, 0, 0, 0, 0,      /* query / createElement / appendChild / set+get attr */
    0, 0, 0,            /* localStorage */
    0, 0,               /* document.cookie */
    0                   /* fetch(): unavailable in ring 3 (no net syscall yet) */
};

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: js <file.js>\n"); return 1; }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { printf("js: cannot open %s\n", argv[1]); return 1; }

    stat_t st;
    if (fstat(fd, &st) != 0) { printf("js: stat failed\n"); close(fd); return 1; }

    unsigned long sz = st.size;
    char *src = (char *)malloc(sz + 1);
    if (!src) { printf("js: out of memory\n"); close(fd); return 1; }

    long n = read(fd, src, sz);
    close(fd);
    if (n < 0) { printf("js: read failed\n"); free(src); return 1; }
    src[n] = 0;

    char err[160];
    int rc = js_run(src, (uint32_t)n, &host, err, sizeof err);
    if (rc) printf("js: %s\n", err);

    free(src);
    return rc ? 1 : 0;
}
