/* rozlibc — sys/wait.h */
#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <_rozlibc.h>

#define WNOHANG 1

#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (WTERMSIG(s) != 0 && WTERMSIG(s) != 0x7F)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif
