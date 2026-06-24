#include <stdint.h>
#include "commands.h"
#include "kprintf.h"
#include "string.h"
#include "fat32.h"
#include "acpi.h"
#include "apic.h"
#include "smp.h"
#include "hpet.h"
#include "audio.h"
#include "dhcp.h"
#include "net.h"
#include "clipboard.h"
#include "ext2.h"
#include "users.h"
#include "gui.h"
#include "framebuffer.h"

/* ===========================================================================
 *  Hardware / platform shell commands: FAT32 volume, power, SMP, ACPI, audio.
 * ===========================================================================*/

int cmd_fat(int argc, char **argv) {
    if (!fat32_mounted()) { kprintf("fat: no FAT32 volume mounted\n"); return 1; }
    const char *sub = argc > 1 ? argv[1] : "ls";

    if (strcmp(sub, "ls") == 0) {
        const char *path = argc > 2 ? argv[2] : "/";
        fat_dirent ents[64];
        int n = fat32_list(path, ents, 64);
        if (n < 0) { kprintf("fat: cannot list %s\n", path); return 1; }
        kprintf("Volume '%s'  %s\n", fat32_label(), path);
        for (int i = 0; i < n; i++) {
            if (ents[i].attr & FAT_ATTR_DIR)
                kprintf("  <DIR>  %s\n", ents[i].name);
            else
                kprintf("  %6u  %s\n", ents[i].size, ents[i].name);
        }
        char human[12]; sh_human(fat32_free_bytes(), human);
        kprintf("%d entries, %s free\n", n, human);
        return 0;
    }
    if (strcmp(sub, "cat") == 0 && argc > 2) {
        static char buf[16384];
        int n = fat32_read(argv[2], buf, sizeof buf - 1);
        if (n < 0) { kprintf("fat: cannot read %s\n", argv[2]); return 1; }
        buf[n] = 0;
        kprintf("%s\n", buf);
        return 0;
    }
    if (strcmp(sub, "write") == 0 && argc > 3) {
        if (fat32_write(argv[2], argv[3], (uint32_t)strlen(argv[3])) != 0) {
            kprintf("fat: write failed\n"); return 1;
        }
        kprintf("wrote %s (%u bytes)\n", argv[2], (unsigned)strlen(argv[3]));
        return 0;
    }
    if (strcmp(sub, "mkdir") == 0 && argc > 2) {
        if (fat32_mkdir(argv[2]) != 0) { kprintf("fat: mkdir failed\n"); return 1; }
        kprintf("created %s\n", argv[2]);
        return 0;
    }
    kprintf("usage: fat ls [path] | cat FILE | write FILE TEXT | mkdir DIR\n");
    return 1;
}

int cmd_power(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "off") == 0 || strcmp(sub, "shutdown") == 0) {
        kprintf("Powering off via ACPI...\n");
        acpi_poweroff();
        return 0;
    }
    if (strcmp(sub, "reboot") == 0 || strcmp(sub, "restart") == 0) {
        kprintf("Rebooting...\n");
        acpi_reboot();
        return 0;
    }
    kprintf("usage: power off | reboot\n");
    return 1;
}

int cmd_cpus(int argc, char **argv) {
    (void)argc; (void)argv;
    const struct acpi_info *ai = acpi_get();
    kprintf("Processors (ACPI MADT): %d\n", ai->cpu_count);
    for (int i = 0; i < ai->cpu_count; i++)
        kprintf("  CPU%d  LAPIC id %u%s\n", i, ai->cpu_lapic_id[i],
                ai->cpu_lapic_id[i] == lapic_id() ? "  (BSP)" : "");
    kprintf("Online: %d  |  interrupt mode: %s\n",
            smp_cpu_count(), apic_active() ? "APIC" : "PIC");
    return 0;
}

int cmd_acpiinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    const struct acpi_info *ai = acpi_get();
    if (!ai->present) { kprintf("ACPI: not present\n"); return 1; }
    kprintf("ACPI revision %d\n", ai->revision);
    kprintf("  Local APIC base : 0x%lx\n", ai->lapic_addr);
    kprintf("  IO-APICs        : %d\n", ai->ioapic_count);
    for (int i = 0; i < ai->ioapic_count; i++)
        kprintf("    IOAPIC%d @0x%x gsi-base %u\n", i, ai->ioapic[i].address, ai->ioapic[i].gsi_base);
    kprintf("  HPET            : %s", ai->hpet_addr ? "" : "absent\n");
    if (ai->hpet_addr) kprintf("@0x%lx (%lu us uptime)\n", ai->hpet_addr, hpet_us());
    kprintf("  Override entries: %d\n", ai->iso_count);
    return 0;
}

int cmd_dhcp(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Requesting DHCP lease...\n");
    if (dhcp_configure(3000) != 0) { kprintf("dhcp: no response (static config kept)\n"); return 1; }
    return 0;
}

int cmd_play(int argc, char **argv) {
    if (!audio_present()) { kprintf("play: no audio device\n"); return 1; }
    int freq = argc > 1 ? atoi(argv[1]) : 440;
    int ms   = argc > 2 ? atoi(argv[2]) : 400;
    kprintf("Playing %d Hz for %d ms on %s\n", freq, ms, audio_name());
    audio_tone((uint32_t)freq, (uint32_t)ms);
    return 0;
}

/* clip: inspect or load the shared system clipboard from the shell, bridging
 * the CLI with the GUI apps that use Ctrl+C / Ctrl+V. */
int cmd_clip(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "show";
    if (strcmp(sub, "show") == 0) {
        if (clip_len() == 0) { kprintf("clipboard is empty\n"); return 0; }
        kprintf("clipboard (%d bytes):\n%s\n", clip_len(), clip_get());
        return 0;
    }
    if (strcmp(sub, "clear") == 0) { clip_set("", 0); kprintf("clipboard cleared\n"); return 0; }
    if (strcmp(sub, "set") == 0 && argc > 2) {
        /* join the remaining args with spaces */
        static char buf[CLIP_CAP]; int n = 0;
        for (int i = 2; i < argc && n < CLIP_CAP - 1; i++) {
            if (i > 2 && n < CLIP_CAP - 1) buf[n++] = ' ';
            for (const char *p = argv[i]; *p && n < CLIP_CAP - 1; p++) buf[n++] = *p;
        }
        buf[n] = 0; clip_set(buf, n);
        kprintf("copied %d bytes to clipboard\n", n);
        return 0;
    }
    kprintf("usage: clip show | set TEXT... | clear\n");
    return 1;
}

/* ext2: read files/dirs off a mounted ext2 volume (e.g. a Linux USB stick). */
int cmd_ext2(int argc, char **argv) {
    if (!ext2_mounted()) { kprintf("ext2: no ext2 volume mounted\n"); return 1; }
    const char *sub = argc > 1 ? argv[1] : "ls";
    if (strcmp(sub, "ls") == 0) {
        const char *path = argc > 2 ? argv[2] : "/";
        ext2_dirent ents[128];
        int n = ext2_list(path, ents, 128);
        if (n < 0) { kprintf("ext2: cannot list %s\n", path); return 1; }
        kprintf("ext2 volume '%s'  %s\n", ext2_volume_name(), path);
        for (int i = 0; i < n; i++) {
            if (ents[i].name[0] == '.' && (ents[i].name[1] == 0 ||
                (ents[i].name[1] == '.' && ents[i].name[2] == 0))) continue;
            if (ents[i].is_dir) kprintf("  <DIR>  %s\n", ents[i].name);
            else                kprintf("  %7u  %s\n", ents[i].size, ents[i].name);
        }
        char human[12]; sh_human(ext2_free_bytes(), human);
        kprintf("%d entries, %s free\n", n, human);
        return 0;
    }
    if (strcmp(sub, "cat") == 0 && argc > 2) {
        static char buf[65536];
        int n = ext2_read(argv[2], buf, sizeof buf - 1);
        if (n < 0) { kprintf("ext2: cannot read %s\n", argv[2]); return 1; }
        buf[n] = 0;
        kprintf("%s\n", buf);
        return 0;
    }
    kprintf("usage: ext2 ls [path] | cat FILE\n");
    return 1;
}

/* whoami / users / logout: multi-user account management. */
int cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *u = users_current();
    kprintf("%s%s\n", u[0] ? u : "(not logged in)", users_is_root() ? "  [root]" : "");
    return 0;
}

int cmd_users(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "list";
    if (strcmp(sub, "list") == 0) {
        kprintf("Accounts (%d):\n", users_count());
        for (int i = 0; i < users_count(); i++) {
            const user_t *u = users_get(i);
            kprintf("  %-16s uid=%d%s\n", u->name, u->uid,
                    strcmp(u->name, users_current()) == 0 ? "  (current)" : "");
        }
        return 0;
    }
    if (strcmp(sub, "add") == 0 && argc > 3) {
        if (!users_is_root() && users_current_uid() != -1) { kprintf("users: must be root to add\n"); return 1; }
        int uid = 1000 + users_count();
        if (users_add(argv[2], argv[3], uid) != 0) { kprintf("users: add failed (exists?)\n"); return 1; }
        kprintf("added '%s' (uid %d)\n", argv[2], uid);
        return 0;
    }
    if (strcmp(sub, "passwd") == 0 && argc > 3) {
        if (users_set_password(argv[2], argv[3]) != 0) { kprintf("users: no such user\n"); return 1; }
        kprintf("password changed for '%s'\n", argv[2]);
        return 0;
    }
    if (strcmp(sub, "su") == 0 && argc > 3) {
        int uid = users_auth(argv[2], argv[3]);
        if (uid < 0) { kprintf("su: authentication failed\n"); return 1; }
        users_login(uid);
        kprintf("now acting as '%s'\n", argv[2]);
        return 0;
    }
    kprintf("usage: users list | add NAME PASS | passwd NAME PASS | su NAME PASS\n");
    return 1;
}

int cmd_logout(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!fb_present()) { kprintf("logout: only available in the desktop session\n"); return 1; }
    kprintf("logging out...\n");
    gui_logout();
    return 0;
}

/* crash: deliberately trigger a CPU exception to exercise the panic/crash-dump
 * path (register grid + page-fault decode + backtrace). Debugging aid. */
int cmd_crash(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "null") == 0) { volatile int *p = (int *)0; *p = 1; }
    else if (strcmp(sub, "div") == 0) { volatile int z = 0; volatile int x = 1 / z; (void)x; }
    else if (strcmp(sub, "ud")  == 0) { __asm__ volatile("ud2"); }
    else kprintf("usage: crash null | div | ud   (triggers a kernel panic)\n");
    return 0;
}
