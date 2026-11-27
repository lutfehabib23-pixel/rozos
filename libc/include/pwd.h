/* rozlibc — pwd.h (ধাপ ১৬ক): /etc/passwd থেকে; ফাইল না থাকলে শুধু root (uid 0) */
#ifndef _PWD_H
#define _PWD_H
#ifdef __cplusplus
extern "C" {
#endif
#include <_rozlibc.h>

struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

struct passwd *getpwnam(const char *name);
struct passwd *getpwuid(uid_t uid);
int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t n, struct passwd **res);
int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t n, struct passwd **res);
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);

#ifdef __cplusplus
}
#endif
#endif
