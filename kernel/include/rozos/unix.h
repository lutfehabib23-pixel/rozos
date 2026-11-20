/* rozOS — Unix domain socket (ধাপ ১৪), net/unix.c দেখুন */
#ifndef ROZOS_UNIX_H
#define ROZOS_UNIX_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>

#define AF_UNIX_K      1
#define UNIX_PATH_MAX  108

int     unix_create(int type, struct vnode **out);
int     unix_is_vnode(struct vnode *vn);
int     unix_bind(struct vnode *vn, const char *path);
int     unix_listen(struct vnode *vn, int backlog);
int     unix_connect(struct vnode *vn, const char *path);
int     unix_accept(struct vnode *vn, struct vnode **out, int nonblock);
int64_t unix_send(struct vnode *vn, const void *buf, size_t len, int dontwait);
int64_t unix_recv(struct vnode *vn, void *buf, size_t len, int dontwait);
int     unix_shutdown(struct vnode *vn, int how);
int     unix_set_nonblock(struct vnode *vn, int on);

#endif
