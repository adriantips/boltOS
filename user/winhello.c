/* A real, freestanding Windows x86-64 console program. It imports kernel32
 * functions (resolved against BoltOS's in-kernel kernel32 shim by the PE loader)
 * and is linked into a genuine PE32+ executable by the build. No CRT. */
#include <stdint.h>

typedef uint64_t HANDLE;
typedef uint32_t DWORD;
typedef int      BOOL;

__declspec(dllimport) HANDLE GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) BOOL   WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *reserved);
__declspec(dllimport) void   ExitProcess(DWORD code);

static DWORD slen(const char *s) { DWORD n = 0; while (s[n]) n++; return n; }

static void say(HANDLE h, const char *s) { DWORD w; WriteConsoleA(h, s, slen(s), &w, 0); }

void entry(void) {
    HANDLE out = GetStdHandle((DWORD)-11);          /* STD_OUTPUT_HANDLE */
    say(out, "Hello from a real Windows PE32+ executable!\r\n");
    say(out, "Running natively under BoltOS via its PE loader + kernel32 shim.\r\n");

    /* prove it is really executing code: print 5 squares */
    char line[32];
    for (int i = 1; i <= 5; i++) {
        int v = i * i, p = 0;
        char tmp[8]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        line[p++] = ' '; line[p++] = ' ';
        line[p++] = (char)('0' + i); line[p++] = '^'; line[p++] = '2'; line[p++] = '='; line[p++] = ' ';
        while (t) line[p++] = tmp[--t];
        line[p++] = '\r'; line[p++] = '\n'; line[p] = 0;
        say(out, line);
    }
    ExitProcess(7);
}
