#include <stdint.h>
#include "shell.h"
#include "commands.h"
#include "kprintf.h"
#include "console.h"
#include "keyboard.h"
#include "fs.h"
#include "pit.h"
#include "string.h"

#define LINESZ  256
#define MAXARG  24

/* ===========================================================================
 *  Shared helpers (declared in commands.h)
 * ===========================================================================*/
void sh_utoa(uint64_t v, char *out) {
    char tmp[24]; int i = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0;
    while (i--) out[j++] = tmp[i];
    out[j] = 0;
}

void sh_pad(const char *s, int width) {
    int n = 0;
    while (s[n]) { kputc(s[n]); n++; }
    for (; n < width; n++) kputc(' ');
}

void sh_human(uint64_t b, char *out) {
    if (b < 1024) { sh_utoa(b, out); strcat(out, "B"); return; }
    uint64_t div; const char *unit;
    if      (b < 1024ull * 1024)             { div = 1024ull;             unit = "K"; }
    else if (b < 1024ull * 1024 * 1024)      { div = 1024ull * 1024;      unit = "M"; }
    else                                     { div = 1024ull * 1024 * 1024;unit = "G"; }
    uint64_t whole = b / div;
    uint64_t tenth = ((b % div) * 10) / div;
    char w[24], t[4];
    sh_utoa(whole, w); sh_utoa(tenth, t);
    strcpy(out, w); strcat(out, "."); strcat(out, t); strcat(out, unit);
}

uint64_t sh_uptime_ms(void) {
    uint32_t hz = pit_hz() ? pit_hz() : 1000;
    return pit_ticks() * 1000ull / hz;
}

int sh_readline(char *buf, int cap) {
    int len = 0;
    for (;;) {
        char c = kbd_getc();
        if (c == '\n') { kputc('\n'); buf[len] = 0; return len; }
        if (c == '\b') { if (len) { len--; kputc('\b'); } continue; }
        if ((unsigned char)c >= 32 && len < cap - 1) { buf[len++] = c; kputc(c); }
    }
}

int sh_yesno(const char *question) {
    char b[8];
    kprintf("%s? [y/N] ", question);
    sh_readline(b, sizeof(b));
    return b[0] == 'y' || b[0] == 'Y';
}

/* ===========================================================================
 *  Core command handlers
 * ===========================================================================*/
int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { if (i > 1) kputc(' '); kprintf("%s", argv[i]); }
    kputc('\n');
    return 0;
}
int cmd_clear(int argc, char **argv) { (void)argc; (void)argv; console_clear(); return 0; }

int cmd_help(int argc, char **argv) {
    if (argc > 1) {
        const command_t *c = cmd_lookup(argv[1]);
        if (!c) { kprintf("help: no such command: %s\n", argv[1]); return 1; }
        kprintf("%s - %s\n", c->name, c->help);
        return 0;
    }
    const char *cat = 0;
    for (int i = 0; i < commands_count; i++) {
        if (!cat || strcmp(cat, commands[i].cat) != 0) {
            cat = commands[i].cat;
            kprintf("\n%s:\n", cat);
        }
        kprintf("  "); sh_pad(commands[i].name, 11); kprintf("%s\n", commands[i].help);
    }
    kprintf("\n%d commands total.  'help NAME' for one command.\n", commands_count);
    return 0;
}

/* ===========================================================================
 *  Command table (grouped by category for help output)
 * ===========================================================================*/
const command_t commands[] = {
    /* File & Directory */
    { "ls",       "File & Directory", "list directory contents",            cmd_ls },
    { "tree",     "File & Directory", "show directory tree",                cmd_tree },
    { "cd",       "File & Directory", "change directory",                   cmd_cd },
    { "pwd",      "File & Directory", "print working directory",            cmd_pwd },
    { "mkdir",    "File & Directory", "create directory",                   cmd_mkdir },
    { "rm",       "File & Directory", "remove file/dir ([-r])",             cmd_rm },
    { "cp",       "File & Directory", "copy file",                          cmd_cp },
    { "mv",       "File & Directory", "move or rename",                     cmd_mv },
    { "find",     "File & Directory", "search for files",                   cmd_find },
    { "touch",    "File & Directory", "create empty file / update mtime",   cmd_touch },
    { "write",    "File & Directory", "write text into a file",             cmd_write },
    { "trash",    "File & Directory", "move file to trash",                 cmd_trash },
    { "recover",  "File & Directory", "restore trashed file",               cmd_recover },

    /* File Inspection */
    { "cat",      "File Inspection",  "print file contents",                cmd_cat },
    { "head",     "File Inspection",  "first lines of file ([-N])",         cmd_head },
    { "tail",     "File Inspection",  "last lines of file ([-N])",          cmd_tail },
    { "hex",      "File Inspection",  "hexadecimal file viewer",            cmd_hex },
    { "meta",     "File Inspection",  "show file metadata",                 cmd_meta },
    { "diff",     "File Inspection",  "compare two files",                  cmd_diff },
    { "grep",     "File Inspection",  "search text in file",                cmd_grep },
    { "checksum", "File Inspection",  "fnv-1a hash of file",                cmd_checksum },
    { "preview",  "File Inspection",  "quick file preview",                 cmd_preview },
    { "count",    "File Inspection",  "count lines/words/chars",            cmd_count },

    /* System Information */
    { "sysinfo",  "System",           "system overview",                    cmd_sysinfo },
    { "cpuinfo",  "System",           "CPU details (CPUID)",                cmd_cpuinfo },
    { "meminfo",  "System",           "memory usage",                       cmd_meminfo },
    { "mem",      "System",           "memory usage (alias)",               cmd_mem },
    { "diskinfo", "System",           "disk/storage status",                cmd_diskinfo },
    { "sync",     "System",           "flush filesystem to disk",           cmd_sync },
    { "uptime",   "System",           "time since boot",                    cmd_uptime },
    { "winrun",   "System",           "run a Windows .exe (PE32+ loader)",   cmd_winrun },
    { "battery",  "System",           "battery status",                     cmd_battery },
    { "sensors",  "System",           "hardware sensors",                   cmd_sensors },
    { "devices",  "System",           "connected hardware (PCI)",           cmd_devices },
    { "version",  "System",           "OS version information",             cmd_version },
    { "health",   "System",           "system health report",              cmd_health },

    /* Process Management */
    { "ps",       "Process",          "list kernel tasks",                  cmd_ps },
    { "kill",     "Process",          "terminate a task",                   cmd_kill },
    { "top",      "Process",          "live task monitor",                  cmd_top },
    { "freeze",   "Process",          "suspend a task",                     cmd_freeze },
    { "resume",   "Process",          "resume a task",                      cmd_resume },
    { "services", "Process",          "list background services",           cmd_services },
    { "service",  "Process",          "manage a service",                   cmd_service },
    { "jobs",     "Process",          "show shell jobs",                    cmd_jobs },
    { "priority", "Process",          "change task priority",               cmd_priority },
    { "monitor",  "Process",          "track resource usage",               cmd_monitor },

    /* Networking */
    { "netinfo",  "Network",          "network configuration",              cmd_netinfo },
    { "ping",     "Network",          "connectivity test",                  cmd_ping },
    { "trace",    "Network",          "route tracing",                      cmd_trace },
    { "ports",    "Network",          "open port viewer",                   cmd_ports },
    { "download", "Network",          "download a file over HTTP",          cmd_download },
    { "browse",   "Network",          "fetch + render a page as text",      cmd_browse },
    { "upload",   "Network",          "upload a file",                      cmd_upload },
    { "wifi",     "Network",          "Wi-Fi management",                   cmd_wifi },
    { "firewall", "Network",          "firewall controls",                  cmd_firewall },
    { "share",    "Network",          "share files over network",           cmd_share },
    { "scan",     "Network",          "discover nearby devices",            cmd_scan },

    /* Bonus / Unique */
    { "focus",    "Bonus",            "block distractions",                 cmd_focus },
    { "snapshot", "Bonus",            "save/restore fs snapshot",           cmd_snapshot },
    { "timeline", "Bonus",            "show activity history",              cmd_timeline },
    { "vault",    "Bonus",            "encrypted file storage (toy)",       cmd_vault },
    { "doctor",   "Bonus",            "diagnose and fix issues",            cmd_doctor },
    { "assistant","Bonus",            "built-in terminal assistant",        cmd_assistant },
    { "sandbox",  "Bonus",            "run a command isolated",             cmd_sandbox },
    { "workspace","Bonus",            "save/restore sessions",              cmd_workspace },
    { "panic",    "Bonus",            "halt and secure the system",         cmd_panic },
    { "story",    "Bonus",            "narrative log of events",            cmd_story },

    /* Core */
    { "help",     "Core",             "show this list",                     cmd_help },
    { "echo",     "Core",             "print text back",                    cmd_echo },
    { "clear",    "Core",             "clear the screen",                   cmd_clear },
    { "python",   "Core",             "BoltPython REPL / run a .py file",    cmd_python },
};
const int commands_count = (int)(sizeof(commands) / sizeof(commands[0]));

const command_t *cmd_lookup(const char *name) {
    for (int i = 0; i < commands_count; i++)
        if (strcmp(commands[i].name, name) == 0) return &commands[i];
    return 0;
}

/* ===========================================================================
 *  Line editor / REPL
 * ===========================================================================*/
static int tokenize(char *line, char **argv) {
    int argc = 0; char *p = line;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc < MAXARG) argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

static void dispatch(char *line) {
    char *argv[MAXARG];
    int argc = tokenize(line, argv);
    if (!argc) return;
    const command_t *c = cmd_lookup(argv[0]);
    if (c) c->fn(argc, argv);
    else   kprintf("unknown command: %s  (try 'help')\n", argv[0]);
}

static void prompt(void) {
    char path[FS_PATH_MAX];
    fs_abspath(fs_cwd(), path, sizeof(path));
    kprintf("bolt:%s> ", path);
}

/* Run one already-typed command line through the normal tokenizer + dispatch.
 * Used by the GUI terminal window, which captures kputc output via a sink. */
void shell_exec_line(char *line) { dispatch(line); }

void shell_run(void) {
    char line[LINESZ];
    uint32_t len = 0;

    kprintf("\nBoltShell ready. %d commands. Type 'help'.\n", commands_count);
    prompt();

    for (;;) {
        int ci = kbd_trygetc();
        if (ci < 0) {                                /* idle: blink + wait for IRQ */
            console_cursor_tick();
            __asm__ volatile("hlt");
            continue;
        }
        char c = (char)ci;
        if (c == '\n') {
            kputc('\n');
            line[len] = 0;
            dispatch(line);
            len = 0;
            prompt();
        } else if (c == '\b') {
            if (len > 0) { len--; kputc('\b'); }     /* erase one char */
        } else if ((unsigned char)c >= 32 && len < LINESZ - 1) {
            line[len++] = c;
            kputc(c);                                /* echo */
        }
    }
}
