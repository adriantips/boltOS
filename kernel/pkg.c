/* ===========================================================================
 *  BoltOS  -  kernel/pkg.c
 *  A minimal package manager. Packages ship in a built-in catalog (BoltPython
 *  scripts). Installing one writes it to /apps/<name>.py and records the name
 *  in /var/lib/pkg/installed; the FS autosaves, so installs survive reboots.
 *  Run an installed package with `python /apps/<name>.py` or `pkg run <name>`.
 * ===========================================================================*/
#include <stdint.h>
#include "commands.h"
#include "kprintf.h"
#include "string.h"
#include "fs.h"
#include "kheap.h"
#include "boltpy.h"

typedef struct { const char *name, *desc, *body; } pkg_t;

/* The catalog. Each body is a self-contained BoltPython program. */
static const pkg_t CATALOG[] = {
    { "hello", "classic greeting",
      "print('Hello from a BoltOS package!')\n" },
    { "fib", "first 15 Fibonacci numbers",
      "a = 0\nb = 1\ni = 0\nwhile i < 15:\n    print(a)\n    t = a + b\n    a = b\n    b = t\n    i = i + 1\n" },
    { "primes", "primes below 50",
      "n = 2\nwhile n < 50:\n    d = 2\n    p = 1\n    while d * d <= n:\n        if n % d == 0:\n            p = 0\n        d = d + 1\n    if p == 1:\n        print(n)\n    n = n + 1\n" },
    { "gcd", "greatest common divisor of 1071 and 462",
      "print(gcd(1071, 462))\n" },
    { "squares", "squares 1..10",
      "i = 1\nwhile i <= 10:\n    print(i * i)\n    i = i + 1\n" },
};
#define NCATALOG ((int)(sizeof(CATALOG) / sizeof(CATALOG[0])))

#define INSTALLED_DB "/var/lib/pkg/installed"

static const pkg_t *find_pkg(const char *name) {
    for (int i = 0; i < NCATALOG; i++)
        if (strcmp(CATALOG[i].name, name) == 0) return &CATALOG[i];
    return 0;
}

static void pkg_path(const char *name, char *out, int cap) {
    out[0] = 0;
    kstrlcat(out, "/apps/", cap);
    kstrlcat(out, name, cap);
    kstrlcat(out, ".py", cap);
}

static int is_installed(const char *name) {
    char p[64]; pkg_path(name, p, sizeof(p));
    return fs_lookup(p) != 0;
}

static void ensure_dirs(void) {
    if (!fs_lookup("/apps"))     fs_create("/apps", 1);
    if (!fs_lookup("/var"))      fs_create("/var", 1);
    if (!fs_lookup("/var/lib"))  fs_create("/var/lib", 1);
    if (!fs_lookup("/var/lib/pkg")) fs_create("/var/lib/pkg", 1);
}

static void db_record(const char *name) {
    ensure_dirs();
    fs_node *f = fs_lookup(INSTALLED_DB);
    if (!f) f = fs_create(INSTALLED_DB, 0);
    if (!f) return;
    char line[40]; line[0] = 0;
    kstrlcat(line, name, sizeof(line));
    kstrlcat(line, "\n", sizeof(line));
    fs_append(f, line, (uint32_t)strlen(line));
}

int cmd_pkg(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "list";

    if (strcmp(sub, "list") == 0) {
        kprintf("Available packages (%d):\n", NCATALOG);
        for (int i = 0; i < NCATALOG; i++)
            kprintf("  %-10s %-34s %s\n", CATALOG[i].name, CATALOG[i].desc,
                    is_installed(CATALOG[i].name) ? "[installed]" : "");
        return 0;
    }
    if (strcmp(sub, "info") == 0 && argc > 2) {
        const pkg_t *p = find_pkg(argv[2]);
        if (!p) { kprintf("pkg: no such package '%s'\n", argv[2]); return 1; }
        kprintf("%s - %s\n  status: %s\n  size:   %u bytes\n",
                p->name, p->desc, is_installed(p->name) ? "installed" : "not installed",
                (unsigned)strlen(p->body));
        return 0;
    }
    if (strcmp(sub, "install") == 0 && argc > 2) {
        const pkg_t *p = find_pkg(argv[2]);
        if (!p) { kprintf("pkg: no such package '%s'\n", argv[2]); return 1; }
        if (is_installed(p->name)) { kprintf("pkg: '%s' already installed\n", p->name); return 0; }
        ensure_dirs();
        char path[64]; pkg_path(p->name, path, sizeof(path));
        fs_node *f = fs_create(path, 0);
        if (!f || fs_write(f, p->body, (uint32_t)strlen(p->body)) != 0) {
            kprintf("pkg: install failed\n"); return 1;
        }
        db_record(p->name);
        kprintf("installed '%s' -> %s\n  run it with: pkg run %s\n", p->name, path, p->name);
        return 0;
    }
    if (strcmp(sub, "remove") == 0 && argc > 2) {
        char path[64]; pkg_path(argv[2], path, sizeof(path));
        if (!fs_lookup(path)) { kprintf("pkg: '%s' is not installed\n", argv[2]); return 1; }
        if (fs_remove(path) != 0) { kprintf("pkg: remove failed\n"); return 1; }
        kprintf("removed '%s'\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "run") == 0 && argc > 2) {
        char path[64]; pkg_path(argv[2], path, sizeof(path));
        fs_node *f = fs_lookup(path);
        if (!f) { kprintf("pkg: '%s' is not installed (try: pkg install %s)\n", argv[2], argv[2]); return 1; }
        char *src = (char *)kmalloc(f->size + 1);
        if (!src) { kprintf("pkg: out of memory\n"); return 1; }
        for (uint32_t i = 0; i < f->size; i++) src[i] = (char)f->data[i];
        src[f->size] = 0;
        int rc = bpy_run(src);
        kfree(src);
        return rc;
    }
    kprintf("usage: pkg list | info NAME | install NAME | remove NAME | run NAME\n");
    return 1;
}
