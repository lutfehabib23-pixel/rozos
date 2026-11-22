/* rozlibc — sys/file.h (ধাপ ১৬ক): flock — একজনই লেখে, তাই লক সবসময় পাওয়া যায় */
#ifndef _SYS_FILE_H
#define _SYS_FILE_H
#include <fcntl.h>
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int flock(int fd, int op);
#endif
