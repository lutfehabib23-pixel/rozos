/* rozlibc — setjmp.h (rbx, rbp, r12-r15, rsp, rip) */
#ifndef _SETJMP_H
#define _SETJMP_H
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* ধাপ ১৬ক: BSD/POSIX রূপ। ধাপ ১৬খ-১: sigsetjmp(env, 1) signal mask-ও রাখে
 * (env[8] = savemask, env[9] = mask), siglongjmp সেটা ফেরায় */
typedef unsigned long sigjmp_buf[10];
int  _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
int  sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif
