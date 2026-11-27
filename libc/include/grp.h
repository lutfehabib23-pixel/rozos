/* rozlibc — grp.h (ধাপ ১৬ক): /etc/group থেকে; না থাকলে শুধু root (gid 0) */
#ifndef _GRP_H
#define _GRP_H
#ifdef __cplusplus
extern "C" {
#endif
#include <_rozlibc.h>

struct group {
    char  *gr_name;
    char  *gr_passwd;
    gid_t  gr_gid;
    char **gr_mem;
};

struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
int getgrnam_r(const char *name, struct group *g, char *buf, size_t n, struct group **res);
int getgrgid_r(gid_t gid, struct group *g, char *buf, size_t n, struct group **res);
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int  initgroups(const char *user, gid_t gid);
int  setgroups(size_t n, const gid_t *list);

#ifdef __cplusplus
}
#endif
#endif
