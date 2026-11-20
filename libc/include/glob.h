/* rozlibc — glob.h (ধাপ ১১): wildcard শুধু পথের শেষ অংশে চলে (যেমন dir/x?.c) */
#ifndef _GLOB_H
#define _GLOB_H

#include <stddef.h>

typedef struct {
    size_t gl_pathc;      /* কয়টা মিলল */
    char **gl_pathv;      /* নামগুলো, শেষে NULL */
    size_t gl_offs;
} glob_t;

#define GLOB_ERR      0x01
#define GLOB_MARK     0x02
#define GLOB_NOSORT   0x04
#define GLOB_NOCHECK  0x10
#define GLOB_NOESCAPE 0x40

#define GLOB_NOSPACE  1
#define GLOB_ABORTED  2
#define GLOB_NOMATCH  3

int  glob(const char *pattern, int flags, int (*errfunc)(const char *path, int err), glob_t *g);
void globfree(glob_t *g);

#endif
