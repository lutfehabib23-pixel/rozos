/* rozOS — futex (ধাপ ১৬খ-২): pthread-এর mutex/cond/join-এর ভিত্তি
 *
 * FUTEX_WAIT(addr, val): *addr এখনো val হলে ঘুম, যতক্ষণ না কেউ FUTEX_WAKE(addr) দেয়
 * (বা সময়সীমা, বা signal)। একই প্রসেসের থ্রেডরা একই ঠিকানা-স্থানে, তাই চাবি
 * (প্রসেস, ইউজার-ঠিকানা)। একটা CPU: IF = 0 রেখে মান পড়া আর তালিকায় ঢোকা, তাই WAKE
 * কখনো হারায় না। অপেক্ষার তালিকা একটা স্থির টেবিল (FUTEX_MAX ঘর)।
 *
 * op: WAIT 0, WAKE 1, WAIT_BITSET 9 (সময়সীমা পরম, CLOCK_MONOTONIC), WAKE_BITSET 10;
 * FUTEX_PRIVATE_FLAG (128) আর CLOCK_REALTIME (256) গ্রহণ করা হয় (আলাদা অর্থ নেই)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/proc.h>
#include <rozos/sched.h>
#include <rozos/syscall.h>
#include <rozos/uaccess.h>
#include <rozos/timer.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <arch/x86_64/cpu.h>

#define FUTEX_MAX       256
#define FUTEX_WAIT      0
#define FUTEX_WAKE      1
#define FUTEX_REQUEUE   3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK  ~(128 | 256)

struct waiter {
    struct process *p;           /* NULL = খালি ঘর */
    uint64_t uaddr;
    struct thread *t;
    int woken;
};

static struct waiter table[FUTEX_MAX];

int futex_wake_addr(struct process *p, uint64_t uaddr, int n)
{
    uint64_t fl = cpu_irq_save();
    int i, done = 0;
    for (i = 0; i < FUTEX_MAX && done < n; i++) {
        struct waiter *w = &table[i];
        if (w->p == p && w->uaddr == uaddr && !w->woken) {
            w->woken = 1;
            sched_wake(w->t);
            done++;
        }
    }
    cpu_irq_restore(fl);
    return done;
}

/* requeue: n জন জাগাও, বাকিদের (সর্বোচ্চ n2) uaddr2-তে সরাও */
static int futex_requeue(struct process *p, uint64_t uaddr, int n, uint64_t uaddr2, int n2)
{
    uint64_t fl = cpu_irq_save();
    int i, done = 0, moved = 0;
    for (i = 0; i < FUTEX_MAX; i++) {
        struct waiter *w = &table[i];
        if (w->p != p || w->uaddr != uaddr || w->woken) continue;
        if (done < n) {
            w->woken = 1;
            sched_wake(w->t);
            done++;
        } else if (moved < n2) {
            w->uaddr = uaddr2;
            moved++;
        }
    }
    cpu_irq_restore(fl);
    return done + moved;
}

struct k_timespec { int64_t tv_sec, tv_nsec; };

static int64_t futex_wait(struct process *p, uint64_t uaddr, uint32_t val, uint64_t uts, int absolute)
{
    struct waiter *w = NULL;
    uint64_t fl, deadline = 0;
    uint32_t cur;
    int i;
    int64_t r = 0;

    if (uts) {
        struct k_timespec ts;
        uint64_t ms;
        if (copy_from_user(&ts, (const void *)(uintptr_t)uts, sizeof(ts)) < 0) return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
        ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec + 999999) / 1000000;
        deadline = absolute ? ms : timer_uptime_ms() + ms;     /* CLOCK_MONOTONIC = uptime */
    }
    if (!user_range_ok(uaddr, 4, 0)) return -EFAULT;
    fl = cpu_irq_save();
    cur = *(volatile uint32_t *)(uintptr_t)uaddr;
    if (cur != val) {
        cpu_irq_restore(fl);
        return -EAGAIN;
    }
    for (i = 0; i < FUTEX_MAX; i++) {
        if (!table[i].p) {
            w = &table[i];
            break;
        }
    }
    if (!w) {
        cpu_irq_restore(fl);
        return -ENOMEM;
    }
    w->p = p;
    w->uaddr = uaddr;
    w->t = sched_current();
    w->woken = 0;
    for (;;) {
        if (w->woken) break;
        if (sched_should_abort()) { r = -EINTR; break; }
        if (uts) {
            uint64_t now = timer_uptime_ms();
            if (now >= deadline) { r = -ETIMEDOUT; break; }
            cpu_irq_restore(fl);
            sched_sleep_ms(deadline - now);      /* wake এলে আগেই ফেরে */
            fl = cpu_irq_save();
        } else {
            sched_block();
        }
    }
    if (w->woken) r = 0;
    w->p = NULL;
    cpu_irq_restore(fl);
    return r;
}

static int64_t sys_futex(struct interrupt_frame *f, uint64_t uaddr, uint64_t op, uint64_t val,
                         uint64_t uts, uint64_t uaddr2, uint64_t val3)
{
    struct process *p = proc_current();
    int cmd = (int)op & FUTEX_CMD_MASK;
    (void)f; (void)val3;
    if (!p || (uaddr & 3)) return -EINVAL;
    switch (cmd) {
    case FUTEX_WAIT:
        return futex_wait(p, uaddr, (uint32_t)val, uts, 0);
    case FUTEX_WAIT_BITSET:
        return futex_wait(p, uaddr, (uint32_t)val, uts, 1);
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET:
        return futex_wake_addr(p, uaddr, (int)val);
    case FUTEX_REQUEUE:
        return futex_requeue(p, uaddr, (int)val, uaddr2, (int)uts);
    case FUTEX_CMP_REQUEUE: {
        uint32_t cur;
        if (copy_from_user(&cur, (const void *)(uintptr_t)uaddr, 4) < 0) return -EFAULT;
        if (cur != (uint32_t)val3) return -EAGAIN;
        return futex_requeue(p, uaddr, (int)val, uaddr2, (int)uts);
    }
    }
    return -ENOSYS;
}

void futex_init(void)
{
    syscall_register(202, sys_futex, "futex");
}
