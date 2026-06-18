#include "ulibc.h"

/* BoltOS native ring-3 demo. Exercises every new subsystem:
 *   - loaded from /bin/hello by the ELF64 loader
 *   - prints via write(1) through the VFS /dev/console
 *   - malloc() grows the heap via SYS_BRK
 *   - mmap() maps an anonymous page
 *   - open()/read()/close() reads /proc/meminfo through the VFS
 *   - returns -> crt0 calls exit()
 */
int main(int argc, char **argv) {
    printf("hello from ring3 (pid=%d)\n", getpid());
    printf("argc=%d argv0=%s\n", argc, argc ? argv[0] : "");

    char *buf = (char *)malloc(64);
    if (buf) {
        for (int i = 0; i < 10; i++) buf[i] = (char)('A' + i);
        buf[10] = 0;
        printf("malloc(64) -> %p : %s\n", buf, buf);
    }

    int *page = (int *)mmap(0, 4096, PROT_READ | PROT_WRITE, 0, -1);
    if (page != (void *)-1) {
        page[0] = 0xBEEF;
        printf("mmap page @%p first word=0x%x\n", page, page[0]);
    } else {
        printf("mmap failed\n");
    }

    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) {
        char info[256];
        long n = read(fd, info, sizeof(info) - 1);
        if (n > 0) { info[n] = 0; printf("--- /proc/meminfo ---\n%s", info); }
        close(fd);
    }

    printf("hello: done, exiting 0\n");
    return 0;
}
