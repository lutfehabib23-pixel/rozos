/* rozOS — pseudo-terminal (ধাপ ১৪), drivers/console/pty.c দেখুন */
#ifndef ROZOS_PTY_H
#define ROZOS_PTY_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>

#define PTY_TIOCROZDETECT       0x54F0   /* sys_fs.c-র TIOCROZDETECT — PTY-তে শুধু মাপ */
#define PTY_TIOCROZDETECT_FORCE 0x54F1

int  pty_open(int fds[2]);               /* fds[0] master, fds[1] slave; 0 বা -ERRNO */
int  pty_is_vnode(struct vnode *vn);
int  pty_ioctl(struct vnode *vn, uint64_t req, uint64_t arg,
               int (*copy_in)(void *, const void *, size_t),
               int (*copy_out)(void *, const void *, size_t));
int  pty_count(void);

#endif
