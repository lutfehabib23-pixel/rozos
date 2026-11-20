/* rozOS — shared memory (ধাপ ১৩): shm_open / mmap(MAP_SHARED) / shm_unlink
 * বিস্তারিত kernel/mm/shm.c-এর মাথায়। */
#ifndef ROZOS_SHM_H
#define ROZOS_SHM_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>
#include <rozos/vmm.h>

#define SHM_NAME_MAX 63
#define SHM_MAX_SIZE (256ULL * 1024 * 1024)   /* একটা বস্তু সর্বোচ্চ 256 MiB */

struct shm_obj;

void shm_init(void);                          /* vmm-এ hook বসানো */
int  shm_open_fd(const char *name, int flags);   /* fd বা -ERRNO */
int  shm_unlink_name(const char *name);
struct shm_obj *shm_from_vnode(struct vnode *vn);   /* shm-এর vnode না হলে NULL */
struct shm_obj *shm_anon_create(uint64_t size);     /* MAP_SHARED|MAP_ANONYMOUS */
void shm_anon_done(struct shm_obj *o);
/* off (পেজ-সীমায়) থেকে len বাইট sp-এর va-তে VMM_SHARED দিয়ে ম্যাপ */
int  shm_map(struct shm_obj *o, struct vmm_space *sp, uint64_t va, uint64_t off,
             uint64_t len, uint32_t flags);
void shm_dump(void);                          /* kmon: shm */

#endif
