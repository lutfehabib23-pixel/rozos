/* rozlibc — sys/mman.h */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#ifdef __cplusplus
extern "C" {
#endif

#include <_rozlibc.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);

/* ধাপ ১৩: shared memory — দুই প্রসেস একই মেমোরি দেখে
 *   int fd = shm_open("/mybuf", O_CREAT | O_RDWR, 0600);
 *   ftruncate(fd, 4096);
 *   char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
 *   ... অন্য প্রসেস একই নামে shm_open + mmap করলে একই বাইট দেখে ...
 *   shm_unlink("/mybuf");   // নাম মোছা; শেষ ম্যাপ/fd বন্ধ হলে মেমোরি ফেরত
 * MAP_SHARED | MAP_ANONYMOUS: নামহীন — fork-এর পর মা-সন্তান ভাগ করে।
 * /dev/fb0-ও MAP_SHARED দিয়ে ম্যাপ হয় (<sys/fb.h>)। */
int   shm_open(const char *name, int oflag, mode_t mode);
int   shm_unlink(const char *name);

#ifdef __cplusplus
}
#endif
#endif
