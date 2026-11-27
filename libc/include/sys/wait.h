/* rozlibc — sys/wait.h */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H
#ifdef __cplusplus
extern "C" {
#endif

#include <_rozlibc.h>

#define WNOHANG 1
#define WUNTRACED 2            /* ধাপ ১৯ছ: job control নেই — শুধু নাম (থামানো প্রসেস হয় না) */
#define WCONTINUED 8

#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7F)
#define WIFSTOPPED(s)  (((s) & 0xFF) == 0x7F)   /* ধাপ ১৯ছ (rozOS-এ থামানো প্রসেস হয় না) */
#define WSTOPSIG(s)    WEXITSTATUS(s)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#ifdef __cplusplus
}
#endif
#endif
