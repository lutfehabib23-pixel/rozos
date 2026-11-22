/* rozlibc — dlfcn.h (ধাপ ১৬ক): rozOS-এ এখনো shared library নেই — dlopen সবসময়
 * ব্যর্থ হয় (dlerror বলে দেয় কেন); প্রোগ্রাম যেন কম্পাইল হয় সেজন্য */
#ifndef _DLFCN_H
#define _DLFCN_H

#define RTLD_LAZY     1
#define RTLD_NOW      2
#define RTLD_NOLOAD   4
#define RTLD_NODELETE 0x1000
#define RTLD_GLOBAL   0x100
#define RTLD_LOCAL    0
#define RTLD_DEFAULT  ((void *)0)
#define RTLD_NEXT     ((void *)-1)

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int   dlclose(void *handle);
char *dlerror(void);
int   dladdr(const void *addr, Dl_info *info);

#endif
