#pragma once
#include <stdint.h>

/* ===========================================================================
 *  BoltShell command table + shared helpers.
 *  A command handler gets a parsed argv (argv[0] is the command name) and
 *  returns 0 on success, non-zero on error.
 * ===========================================================================*/

typedef int (*cmd_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    const char *cat;     /* category label, for grouped help */
    const char *help;    /* one-line summary */
    cmd_fn      fn;
} command_t;

extern const command_t  commands[];
extern const int        commands_count;
const command_t        *cmd_lookup(const char *name);

/* ---- shared shell helpers (kernel/shell.c) ------------------------------ */
void     sh_pad(const char *s, int width);     /* print s, pad w/ spaces to width */
void     sh_human(uint64_t bytes, char *out);  /* "1.2K" / "16.0M"; out>=12 */
void     sh_utoa(uint64_t v, char *out);       /* unsigned -> decimal string */
uint64_t sh_uptime_ms(void);
int      sh_readline(char *buf, int cap);      /* blocking line read w/ echo */
int      sh_yesno(const char *question);       /* prompt, 1=yes 0=no */

/* ---- handlers: Hardware / Platform -------------------------------------- */
int cmd_fat(int, char **);    int cmd_power(int, char **);  int cmd_cpus(int, char **);
int cmd_acpiinfo(int, char **);int cmd_play(int, char **);  int cmd_dhcp(int, char **);
int cmd_clip(int, char **);   int cmd_crash(int, char **);
int cmd_whoami(int, char **); int cmd_users(int, char **);  int cmd_logout(int, char **);
int cmd_pkg(int, char **);    int cmd_ext2(int, char **);

/* ---- handlers: File & Directory ----------------------------------------- */
int cmd_ls(int, char **);     int cmd_tree(int, char **);   int cmd_cd(int, char **);
int cmd_mkdir(int, char **);  int cmd_rm(int, char **);     int cmd_cp(int, char **);
int cmd_mv(int, char **);     int cmd_find(int, char **);   int cmd_trash(int, char **);
int cmd_recover(int, char **);int cmd_pwd(int, char **);    int cmd_touch(int, char **);
int cmd_write(int, char **);

/* ---- handlers: File Inspection ------------------------------------------ */
int cmd_cat(int, char **);    int cmd_head(int, char **);   int cmd_tail(int, char **);
int cmd_hex(int, char **);    int cmd_meta(int, char **);   int cmd_diff(int, char **);
int cmd_grep(int, char **);   int cmd_checksum(int, char **);int cmd_preview(int, char **);
int cmd_count(int, char **);

/* ---- handlers: System Information --------------------------------------- */
int cmd_sysinfo(int, char **);int cmd_cpuinfo(int, char **);int cmd_meminfo(int, char **);
int cmd_diskinfo(int, char **);int cmd_uptime(int, char **);int cmd_battery(int, char **);
int cmd_sensors(int, char **);int cmd_devices(int, char **);int cmd_version(int, char **);
int cmd_health(int, char **); int cmd_sync(int, char **);

/* ---- handlers: Process Management --------------------------------------- */
int cmd_ps(int, char **);     int cmd_kill(int, char **);   int cmd_top(int, char **);
int cmd_freeze(int, char **); int cmd_resume(int, char **); int cmd_services(int, char **);
int cmd_service(int, char **);int cmd_jobs(int, char **);   int cmd_priority(int, char **);
int cmd_monitor(int, char **); int cmd_winrun(int, char **); int cmd_compile(int, char **);

/* ---- handlers: Networking ----------------------------------------------- */
int cmd_netinfo(int, char **);int cmd_ping(int, char **);   int cmd_trace(int, char **);
int cmd_ports(int, char **);  int cmd_download(int, char **);int cmd_upload(int, char **);
int cmd_wifi(int, char **);   int cmd_firewall(int, char **);int cmd_share(int, char **);
int cmd_scan(int, char **);   int cmd_browse(int, char **); int cmd_domq(int, char **);
int cmd_domlayout(int, char **);

/* ---- handlers: Bonus / Unique ------------------------------------------- */
int cmd_focus(int, char **);  int cmd_snapshot(int, char **);int cmd_timeline(int, char **);
int cmd_vault(int, char **);  int cmd_doctor(int, char **); int cmd_assistant(int, char **);
int cmd_sandbox(int, char **);int cmd_workspace(int, char **);int cmd_panic(int, char **);
int cmd_story(int, char **);

/* ---- handlers: Core ----------------------------------------------------- */
int cmd_help(int, char **);   int cmd_echo(int, char **);   int cmd_clear(int, char **);
int cmd_mem(int, char **);    int cmd_python(int, char **);
int cmd_js(int, char **);
