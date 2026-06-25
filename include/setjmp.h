#pragma once
/* <setjmp.h> -- non-local jumps (libc/setjmp.S). Stores the callee-saved set
 * (rbx,rbp,r12-r15), rsp and the return rip. Used by libjpeg/libpng-style error
 * handling in image codecs. 8 x uint64. */
typedef unsigned long __jmp_buf[8];
typedef struct { __jmp_buf __jb; } jmp_buf[1];
typedef jmp_buf sigjmp_buf;

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define setjmp(env) setjmp(env)
/* signals are absent; sigsetjmp ignores the savemask */
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val)     longjmp(env, val)
int  _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
