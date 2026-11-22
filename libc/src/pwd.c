/* rozlibc — pwd.h ও grp.h (ধাপ ১৬ক)
 *
 * /etc/passwd: name:passwd:uid:gid:gecos:dir:shell
 * /etc/group:  name:passwd:gid:user1,user2
 * ফাইল না থাকলে (বা খুঁজে না পেলে uid 0) ভেতরের একটা root এন্ট্রি — rozOS-এ এখন
 * একজনই ব্যবহারকারী (root), তাই getpwuid(getuid()) সবসময় কিছু ফেরত দেয়। */

#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define LINE_SZ 512
#define MAX_MEM 32

/* ---------- passwd ---------- */

static FILE *pw_f;
static struct passwd pw_static;
static char pw_buf[LINE_SZ];

static char *field(char **p)
{
    char *s = *p, *c;
    if (!s) return (char *)"";
    c = strchr(s, ':');
    if (c) { *c = '\0'; *p = c + 1; }
    else *p = NULL;
    return s;
}

static int parse_pw(char *line, struct passwd *pw)
{
    char *p = line;
    line[strcspn(line, "\r\n")] = '\0';
    if (!*line || *line == '#') return -1;
    pw->pw_name = field(&p);
    pw->pw_passwd = field(&p);
    pw->pw_uid = (uid_t)strtoul(field(&p), NULL, 10);
    pw->pw_gid = (gid_t)strtoul(field(&p), NULL, 10);
    pw->pw_gecos = field(&p);
    pw->pw_dir = field(&p);
    pw->pw_shell = field(&p);
    return 0;
}

static struct passwd *root_pw(struct passwd *pw, char *buf, size_t n)
{
    const char *def = "root:x:0:0:root:/home:/bin/sh";
    if (n < strlen(def) + 1) return NULL;
    strcpy(buf, def);
    parse_pw(buf, pw);
    return pw;
}

void setpwent(void)
{
    if (pw_f) rewind(pw_f);
}

void endpwent(void)
{
    if (pw_f) fclose(pw_f);
    pw_f = NULL;
}

struct passwd *getpwent(void)
{
    if (!pw_f) {
        pw_f = fopen("/etc/passwd", "r");
        if (!pw_f) return NULL;
    }
    while (fgets(pw_buf, sizeof(pw_buf), pw_f)) {
        if (parse_pw(pw_buf, &pw_static) == 0) return &pw_static;
    }
    return NULL;
}

static int pw_find(const char *name, uid_t uid, struct passwd *pw, char *buf, size_t n,
                   struct passwd **res)
{
    FILE *f = fopen("/etc/passwd", "r");
    *res = NULL;
    if (f) {
        while (fgets(buf, (int)n, f)) {
            if (parse_pw(buf, pw) < 0) continue;
            if (name ? !strcmp(pw->pw_name, name) : pw->pw_uid == uid) {
                fclose(f);
                *res = pw;
                return 0;
            }
        }
        fclose(f);
    }
    if ((name && !strcmp(name, "root")) || (!name && uid == 0)) {
        *res = root_pw(pw, buf, n);
        return *res ? 0 : ERANGE;
    }
    return 0;
}

int getpwnam_r(const char *name, struct passwd *pw, char *buf, size_t n, struct passwd **res)
{
    return pw_find(name, 0, pw, buf, n, res);
}

int getpwuid_r(uid_t uid, struct passwd *pw, char *buf, size_t n, struct passwd **res)
{
    return pw_find(NULL, uid, pw, buf, n, res);
}

struct passwd *getpwnam(const char *name)
{
    struct passwd *r;
    pw_find(name, 0, &pw_static, pw_buf, sizeof(pw_buf), &r);
    return r;
}

struct passwd *getpwuid(uid_t uid)
{
    struct passwd *r;
    pw_find(NULL, uid, &pw_static, pw_buf, sizeof(pw_buf), &r);
    return r;
}

/* ---------- group ---------- */

static FILE *gr_f;
static struct group gr_static;
static char gr_buf[LINE_SZ];
static char *gr_mem_static[MAX_MEM + 1];

/* buf-এর শেষে সদস্য-তালিকার জায়গা; mem ব্যবহারকারীর দেওয়া (বা static) */
static int parse_gr(char *line, struct group *g, char **mem, int maxmem)
{
    char *p = line, *list;
    int k = 0;
    line[strcspn(line, "\r\n")] = '\0';
    if (!*line || *line == '#') return -1;
    g->gr_name = field(&p);
    g->gr_passwd = field(&p);
    g->gr_gid = (gid_t)strtoul(field(&p), NULL, 10);
    list = field(&p);
    while (list && *list && k < maxmem) {
        char *c = strchr(list, ',');
        if (c) *c = '\0';
        if (*list) mem[k++] = list;
        list = c ? c + 1 : NULL;
    }
    mem[k] = NULL;
    g->gr_mem = mem;
    return 0;
}

void setgrent(void)
{
    if (gr_f) rewind(gr_f);
}

void endgrent(void)
{
    if (gr_f) fclose(gr_f);
    gr_f = NULL;
}

struct group *getgrent(void)
{
    if (!gr_f) {
        gr_f = fopen("/etc/group", "r");
        if (!gr_f) return NULL;
    }
    while (fgets(gr_buf, sizeof(gr_buf), gr_f)) {
        if (parse_gr(gr_buf, &gr_static, gr_mem_static, MAX_MEM) == 0) return &gr_static;
    }
    return NULL;
}

static int gr_find(const char *name, gid_t gid, struct group *g, char *buf, size_t n,
                   struct group **res)
{
    FILE *f;
    char **mem;
    size_t memsz = sizeof(char *) * (MAX_MEM + 1);
    *res = NULL;
    if (n < memsz + 64) return ERANGE;
    /* buf-এর শুরুতে পয়েন্টার-তালিকা (সারিবদ্ধ), পরে লাইন */
    mem = (char **)(void *)buf;
    buf += memsz;
    n -= memsz;
    f = fopen("/etc/group", "r");
    if (f) {
        while (fgets(buf, (int)n, f)) {
            if (parse_gr(buf, g, mem, MAX_MEM) < 0) continue;
            if (name ? !strcmp(g->gr_name, name) : g->gr_gid == gid) {
                fclose(f);
                *res = g;
                return 0;
            }
        }
        fclose(f);
    }
    if ((name && !strcmp(name, "root")) || (!name && gid == 0)) {
        strcpy(buf, "root:x:0:root");
        parse_gr(buf, g, mem, MAX_MEM);
        *res = g;
    }
    return 0;
}

int getgrnam_r(const char *name, struct group *g, char *buf, size_t n, struct group **res)
{
    return gr_find(name, 0, g, buf, n, res);
}

int getgrgid_r(gid_t gid, struct group *g, char *buf, size_t n, struct group **res)
{
    return gr_find(NULL, gid, g, buf, n, res);
}

static char gr_rbuf[LINE_SZ + sizeof(char *) * (MAX_MEM + 1)] __attribute__((aligned(8)));

struct group *getgrnam(const char *name)
{
    struct group *r;
    gr_find(name, 0, &gr_static, gr_rbuf, sizeof(gr_rbuf), &r);
    return r;
}

struct group *getgrgid(gid_t gid)
{
    struct group *r;
    gr_find(NULL, gid, &gr_static, gr_rbuf, sizeof(gr_rbuf), &r);
    return r;
}

int initgroups(const char *user, gid_t gid)
{
    (void)user;
    (void)gid;
    return 0;
}

int setgroups(size_t n, const gid_t *list)
{
    (void)list;
    if (n > 1) { errno = EPERM; return -1; }
    return 0;
}

/* ---------- লগইন নাম ---------- */

char *getlogin(void)
{
    static char name[64];
    struct passwd *pw;
    const char *u = getenv("USER");
    if (!u || !*u) u = getenv("LOGNAME");
    if (u && *u) {
        strncpy(name, u, sizeof(name) - 1);
        return name;
    }
    pw = getpwuid(getuid());
    if (!pw) return NULL;
    strncpy(name, pw->pw_name, sizeof(name) - 1);
    return name;
}

int getlogin_r(char *buf, size_t n)
{
    const char *u = getlogin();
    if (!u) return ENOENT;
    if (strlen(u) + 1 > n) return ERANGE;
    strcpy(buf, u);
    return 0;
}
