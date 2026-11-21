/* rozOS — উদাহরণ কার্নেল মডিউল (ধাপ ১৫ক)
 *
 *   insmod hello          → কার্নেলে ঢোকে, /dev/hello তৈরি করে
 *   cat /dev/hello        → মডিউল থেকে লেখা
 *   echo hi > /dev/hello  → কার্নেল-বার্তায় দেখা যায়
 *   rmmod hello           → /dev/hello চলে যায়, মডিউল বের হয়
 *
 * rozOS-এর ভেতরে নিজে বানানো:
 *   cp /usr/src/modules/hello.c . && edit hello.c
 *   tcc -c -I/usr/src/rozos/kernel/include hello.c -o hello.ko && insmod ./hello.ko
 * মডিউল শুধু রপ্তানি করা কার্নেল-ফাংশন ডাকতে পারে (lsmod -k)। */

#include <rozos/module.h>
#include <rozos/printf.h>
#include <rozos/devfs.h>
#include <rozos/timer.h>
#include <rozos/string.h>

ROZMOD_INFO("hello", "example module: /dev/hello");

static uint64_t loaded_ms;
static unsigned long writes;

static int64_t hello_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    char msg[160];
    int n;
    (void)ctx;
    n = ksnprintf(msg, sizeof(msg), "Hello from a rozOS kernel module!\nloaded %llu s ago, %lu write(s) so far\n",
                  (unsigned long long)((timer_uptime_ms() - loaded_ms) / 1000), writes);
    if (off >= (uint64_t)n) return 0;
    if (len > (size_t)n - off) len = (size_t)n - off;
    memcpy(buf, msg + off, len);
    return (int64_t)len;
}

static int64_t hello_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    char t[64];
    size_t n = len < sizeof(t) - 1 ? len : sizeof(t) - 1;
    (void)ctx;
    (void)off;
    memcpy(t, buf, n);
    t[n] = '\0';
    if (n && t[n - 1] == '\n') t[n - 1] = '\0';
    writes++;
    kprintf("[hello] someone wrote: %s\n", t);
    return (int64_t)len;
}

int rozmod_init(void)
{
    loaded_ms = timer_uptime_ms();
    if (devfs_register("hello", hello_read, hello_write, NULL) < 0) return -1;
    kprintf("[hello] module loaded - try: cat /dev/hello\n");
    return 0;
}

void rozmod_exit(void)
{
    devfs_unregister("hello");
    kprintf("[hello] goodbye\n");
}
