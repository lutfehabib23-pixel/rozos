/* rozlibc — poll(): কয়েকটা fd-এর দিকে একসাথে নজর (ধাপ ১৩)
 *
 *   struct pollfd p[2] = { { 0, POLLIN, 0 }, { mfd, POLLIN, 0 } };
 *   int n = poll(p, 2, 100);     // 100 ms পর্যন্ত অপেক্ষা; -1 = চিরকাল, 0 = শুধু দেখা
 *   if (p[0].revents & POLLIN) ...   // কীবোর্ড/serial-এ কিছু এসেছে
 *
 * কাজ করে: /dev/console, /dev/kbd, /dev/mouse, /dev/serial0, pipe, socket, সাধারণ ফাইল
 * (সাধারণ ফাইল সবসময় তৈরি)। ফেরত: কয়টা fd-এ ঘটনা আছে, 0 = সময় শেষ, -1 = ত্রুটি। */
#ifndef _POLL_H
#define _POLL_H

#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN     0x0001   /* পড়ার মতো কিছু আছে (বা EOF) */
#define POLLPRI    0x0002
#define POLLOUT    0x0004   /* আটকে না গিয়ে লেখা যাবে */
#define POLLERR    0x0008
#define POLLHUP    0x0010   /* অন্য মুখ বন্ধ (pipe/socket) */
#define POLLNVAL   0x0020   /* fd খোলা নেই */
#define POLLRDNORM 0x0040
#define POLLWRNORM 0x0100

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;
    short events;     /* যা জানতে চাই */
    short revents;    /* কার্নেল যা পেল (POLLERR/POLLHUP/POLLNVAL সবসময় আসতে পারে) */
};

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
