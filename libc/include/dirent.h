/* rozlibc — dirent.h (getdents64-এর ওপর) */
#ifndef _DIRENT_H
#define _DIRENT_H

#include <_rozlibc.h>

#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_REG     8

struct dirent {
    ino_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

typedef struct _rozlibc_dir DIR;

DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);
void           rewinddir(DIR *d);
int            dirfd(DIR *d);
DIR           *fdopendir(int fd);     /* ধাপ ১৯ছ: খোলা ডিরেক্টরির fd থেকে (fd DIR-এর হয়ে যায়) */

#endif
