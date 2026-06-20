#include <stdint.h>
#include "commands.h"
#include "sysreg.h"
#include "pmm.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"
#include "console.h"
#include "keyboard.h"
#include "io.h"
#include "fs.h"
#include "pe.h"
#include "kheap.h"

static uint64_t secs_since(uint64_t start) {
    uint32_t hz = pit_hz() ? pit_hz() : 1000;
    return (pit_ticks() - start) / hz;
}

/* -------------------------------- ps ------------------------------------- */
static void ps_header(void) {
    sh_pad("PID", 5); sh_pad("NAME", 12); sh_pad("STATE", 8);
    sh_pad("PRI", 5); kprintf("TIME\n");
}
static void ps_row(task_t *t) {
    char num[16];
    sh_utoa((uint64_t)t->pid, num); sh_pad(num, 5);
    sh_pad(t->name, 12);
    sh_pad(t->state, 8);
    sh_utoa((uint64_t)(t->prio < 0 ? -t->prio : t->prio), num);
    kprintf("%s%s", t->prio < 0 ? "-" : "", num);
    for (int i = (int)strlen(num) + (t->prio < 0 ? 1 : 0); i < 5; i++) kputc(' ');
    kprintf("%lus\n", (unsigned long)secs_since(t->start));
}
int cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    ps_header();
    for (int i = 0; i < task_count(); i++) ps_row(task_get(i));
    kprintf("(%d kernel contexts; BoltOS is a cooperative single-address-space kernel)\n",
            task_count());
    return 0;
}

/* ------------------------------- kill ------------------------------------ */
int cmd_kill(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: kill PID\n"); return 1; }
    task_t *t = task_find_pid(atoi(argv[1]));
    if (!t) { kprintf("kill: no such pid %s\n", argv[1]); return 1; }
    if (t->protect) {
        kprintf("kill: %d (%s) is a core kernel context and cannot be killed\n",
                t->pid, t->name);
        return 1;
    }
    strncpy(t->state, "stop", sizeof(t->state));
    kprintf("killed %d (%s)\n", t->pid, t->name);
    return 0;
}

/* ----------------------------- freeze/resume ----------------------------- */
static int set_state(int argc, char **argv, const char *st, const char *verb) {
    if (argc < 2) { kprintf("usage: %s PID\n", verb); return 1; }
    task_t *t = task_find_pid(atoi(argv[1]));
    if (!t) { kprintf("%s: no such pid %s\n", verb, argv[1]); return 1; }
    strncpy(t->state, st, sizeof(t->state));
    kprintf("%s %d (%s)", verb, t->pid, t->name);
    if (t->protect) kprintf("  [advisory: cooperative kernel has no preemption]");
    kprintf("\n");
    return 0;
}
int cmd_freeze(int argc, char **argv) { return set_state(argc, argv, "froze", "freeze"); }
int cmd_resume(int argc, char **argv) { return set_state(argc, argv, "run",   "resume"); }

/* ----------------------------- priority ---------------------------------- */
int cmd_priority(int argc, char **argv) {
    if (argc < 3) { kprintf("usage: priority PID NICE(-20..19)\n"); return 1; }
    task_t *t = task_find_pid(atoi(argv[1]));
    if (!t) { kprintf("priority: no such pid %s\n", argv[1]); return 1; }
    int n = atoi(argv[2]);
    if (n < -20) n = -20; if (n > 19) n = 19;
    t->prio = n;
    kprintf("pid %d (%s) priority -> %d\n", t->pid, t->name, n);
    return 0;
}

/* -------------------------------- top ------------------------------------ */
int cmd_top(int argc, char **argv) {
    (void)argc; (void)argv;
    for (;;) {
        console_clear();
        uint64_t freep = pmm_free_count(), totp = pmm_total_count();
        kprintf("BoltTop   uptime %lus   mem %lu/%lu MiB free   (press any key to exit)\n",
                (unsigned long)(pit_ticks() / (pit_hz() ? pit_hz() : 1000)),
                (unsigned long)((freep * 4096ull) >> 20),
                (unsigned long)((totp * 4096ull) >> 20));
        kprintf("-----------------------------------------------------------\n");
        ps_header();
        for (int i = 0; i < task_count(); i++) ps_row(task_get(i));

        uint64_t until = pit_ticks() + (pit_hz() ? pit_hz() : 1000);
        while (pit_ticks() < until) {
            if (kbd_trygetc() >= 0) { console_clear(); return 0; }
            hlt();
        }
    }
}

/* ------------------------------ monitor ---------------------------------- */
int cmd_monitor(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 5;
    if (n <= 0) n = 5;
    uint32_t hz = pit_hz() ? pit_hz() : 1000;
    kprintf("monitoring %d samples (1/s, any key aborts):\n", n);
    sh_pad("t", 5); sh_pad("freeMiB", 10); kprintf("ticks\n");
    for (int i = 0; i < n; i++) {
        uint64_t freep = pmm_free_count();
        char num[16];
        sh_utoa((uint64_t)i, num); sh_pad(num, 5);
        sh_utoa((freep * 4096ull) >> 20, num); sh_pad(num, 10);
        kprintf("%lu\n", (unsigned long)pit_ticks());
        uint64_t until = pit_ticks() + hz;
        while (pit_ticks() < until) {
            if (kbd_trygetc() >= 0) { kprintf("aborted\n"); return 0; }
            hlt();
        }
    }
    return 0;
}

/* ----------------------------- services ---------------------------------- */
int cmd_services(int argc, char **argv) {
    (void)argc; (void)argv;
    sh_pad("SERVICE", 12); sh_pad("KIND", 10); sh_pad("STATE", 9); kprintf("UPTIME\n");
    for (int i = 0; i < svc_count(); i++) {
        service_t *s = svc_get(i);
        sh_pad(s->name, 12);
        sh_pad(s->kind, 10);
        sh_pad(s->running ? "running" : "stopped", 9);
        kprintf("%lus\n", (unsigned long)secs_since(s->since));
    }
    return 0;
}

int cmd_service(int argc, char **argv) {
    if (argc < 2) { kprintf("usage: service NAME [start|stop|restart|status]\n"); return 1; }
    service_t *s = svc_find(argv[1]);
    if (!s) { kprintf("service: %s: not found\n", argv[1]); return 1; }
    const char *act = (argc > 2) ? argv[2] : "status";
    if      (strcmp(act, "start")   == 0) { s->running = 1; kprintf("%s started\n", s->name); }
    else if (strcmp(act, "stop")    == 0) { s->running = 0; kprintf("%s stopped\n", s->name); }
    else if (strcmp(act, "restart") == 0) { s->running = 1; kprintf("%s restarted\n", s->name); }
    else kprintf("%s: %s  (%s)\n", s->name, s->running ? "running" : "stopped", s->kind);
    return 0;
}

/* ------------------------------ winrun ----------------------------------- */
/* Run a real Windows PE32+ console .exe through the in-kernel loader. With no
 * argument it runs the embedded demo (user/winhello.c, built into a genuine PE
 * by the mingw toolchain); with a path it loads that .exe from the filesystem. */
extern const uint8_t _binary_winhello_exe_start[];
extern const uint8_t _binary_winhello_exe_end[];

int cmd_winrun(int argc, char **argv) {
    const uint8_t *img; uint32_t sz;
    if (argc > 1) {
        fs_node *n = fs_lookup(argv[1]);
        if (!n || n->is_dir || !n->data || !n->size) { kprintf("winrun: %s: not found\n", argv[1]); return 1; }
        img = n->data; sz = (uint32_t)n->size;
    } else {
        img = _binary_winhello_exe_start;
        sz  = (uint32_t)(_binary_winhello_exe_end - _binary_winhello_exe_start);
    }
    kprintf("[winrun] loading PE32+ image (%u bytes)\n", sz);
    int code = pe_run(img, sz);
    kprintf("[winrun] process exited with code %d\n", code);
    return 0;
}

/* ------------------------------ compile ---------------------------------- */
/* Compile a BoltOS source file (.c/.cpp/.cs/.py) into a real Windows PE32+ .exe.
 *
 * The trick: a runtime stub (user/boltrt.c) with the actual BoltCC/BoltPython
 * front-ends baked in is embedded in the kernel as a PE template. We copy that
 * template, find its payload marker, and write the user's source into it. The
 * result is a genuine Windows console executable that runs on Windows 11 and on
 * BoltOS itself (`winrun out.exe`), since the stub imports only kernel32 — which
 * the in-kernel PE loader shims. */
extern const uint8_t _binary_boltrt_exe_start[];
extern const uint8_t _binary_boltrt_exe_end[];

/* 16-byte payload marker the boltrt stub scans for at run time. */
static const uint8_t BOLTRT_MAGIC[16] = {
    0x42, 0x6F, 0x4C, 0x74, 0x50, 0x41, 0x59, 0x31,
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x13, 0x37
};

uint8_t *pe_build_exe(int lang, const uint8_t *src, uint32_t len, uint32_t *out_sz) {
    if (lang < 0 || lang > 3 || !src) return 0;
    const uint8_t *tpl = _binary_boltrt_exe_start;
    uint32_t tsz = (uint32_t)(_binary_boltrt_exe_end - _binary_boltrt_exe_start);

    /* payload buffer is 64 KiB: 16 magic + 1 lang + 4 len + source + NUL */
    if (21u + len + 1u > 65536u) return 0;

    uint8_t *img = (uint8_t *)kmalloc(tsz);
    if (!img) return 0;
    memcpy(img, tpl, tsz);

    uint32_t off = 0xFFFFFFFFu;
    for (uint32_t i = 0; i + 16 <= tsz; i++) {
        if (img[i] == BOLTRT_MAGIC[0] && memcmp(img + i, BOLTRT_MAGIC, 16) == 0) { off = i; break; }
    }
    if (off == 0xFFFFFFFFu) { kfree(img); return 0; }

    img[off + 16] = (uint8_t)lang;
    img[off + 17] = (uint8_t)(len & 0xFF);
    img[off + 18] = (uint8_t)((len >> 8) & 0xFF);
    img[off + 19] = (uint8_t)((len >> 16) & 0xFF);
    img[off + 20] = (uint8_t)((len >> 24) & 0xFF);
    memcpy(img + off + 21, src, len);
    img[off + 21 + len] = 0;

    if (out_sz) *out_sz = tsz;
    return img;
}

static int compile_lang(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return -1;
    if (!strcmp(dot, ".c"))   return 0;             /* C   */
    if (!strcmp(dot, ".cpp") || !strcmp(dot, ".cc") || !strcmp(dot, ".cxx") ||
        !strcmp(dot, ".cp")  || !strcmp(dot, ".c++")) return 1;   /* C++ */
    if (!strcmp(dot, ".cs"))  return 2;             /* C#  */
    if (!strcmp(dot, ".py"))  return 3;             /* Python */
    return -1;
}

int cmd_compile(int argc, char **argv) {
    if (argc < 2) {
        kprintf("usage: compile SRC [OUT.exe]\n");
        kprintf("  compiles a .c/.cpp/.cs/.py file into a real Windows PE32+ .exe\n");
        kprintf("  the .exe runs on Windows 11 and on BoltOS (winrun OUT.exe)\n");
        return 1;
    }
    fs_node *f = fs_lookup(argv[1]);
    if (!f || f->is_dir || !f->data) { kprintf("compile: not a file: %s\n", argv[1]); return 1; }

    int lang = compile_lang(f->name);
    if (lang < 0) { kprintf("compile: unknown source type (use .c/.cpp/.cs/.py)\n"); return 1; }

    /* derive an output name: base of SRC with the extension replaced by .exe */
    char out[64];
    if (argc >= 3) {
        uint32_t i = 0; for (; argv[2][i] && i < sizeof(out) - 1; i++) out[i] = argv[2][i]; out[i] = 0;
    } else {
        const char *base = strrchr(argv[1], '/'); base = base ? base + 1 : argv[1];
        uint32_t i = 0; for (; base[i] && base[i] != '.' && i < sizeof(out) - 5; i++) out[i] = base[i];
        out[i++] = '.'; out[i++] = 'e'; out[i++] = 'x'; out[i++] = 'e'; out[i] = 0;
    }

    uint32_t tsz = 0;
    uint8_t *img = pe_build_exe(lang, f->data, f->size, &tsz);
    if (!img) {
        if (21u + f->size + 1u > 65536u) kprintf("compile: source too large (max ~64 KiB)\n");
        else kprintf("compile: build failed (out of memory or corrupt stub)\n");
        return 1;
    }

    fs_node *o = fs_lookup(out);
    if (!o) o = fs_create(out, 0);
    if (!o || o->is_dir) { kprintf("compile: cannot write '%s'\n", out); kfree(img); return 1; }
    fs_write(o, img, tsz);
    kfree(img);

    static const char *names[] = { "C", "C++", "C#", "Python" };
    kprintf("compiled %s (%s) -> %s (%u bytes)\n", f->name, names[lang], out, tsz);
    kprintf("  run here:    winrun %s\n", out);
    kprintf("  on Windows:  copy %s out and double-click / run it\n", out);
    return 0;
}

/* ------------------------------- jobs ------------------------------------ */
int cmd_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("no background jobs.\n");
    kprintf("BoltShell runs each command synchronously (no '&' job control yet).\n");
    return 0;
}
