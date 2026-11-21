/* rozlibc — rozOS-এর নিজস্ব সুবিধা (POSIX নয়) */
#ifndef _ROZOS_H
#define _ROZOS_H

/* সরাসরি syscall: ফলাফল কাঁচা (ত্রুটি হলে -ERRNO, errno বসায় না) */
long __syscall6(long n, long a, long b, long c, long d, long e, long f);
long __syscall_int80(long n, long a, long b, long c);   /* int 0x80 পথ (পরীক্ষার জন্য) */
#define __syscall3(n, a, b, c) __syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

/* Linux-এর মতো: -ERRNO হলে errno বসিয়ে -1, নইলে মান */
long __syscall_ret(long r);

#define ROZOS_SYS_KMON 500
#define ROZOS_SYS_NETINFO 501
#define ROZOS_SYS_NETCONFIG 502

int  rozos_kmon(void);                 /* কার্নেল মনিটর; "exit" দিলে ফেরে */

/* ধাপ ৯: নেটওয়ার্ক interface (ঠিকানা network byte order-এ) */
struct rozos_netinfo {
    char     name[8];
    char     driver[16];
    unsigned char mac[6];
    unsigned char link_up;
    unsigned char configured;
    unsigned char dhcp;
    unsigned char reserved[3];
    unsigned int ip, netmask, gateway, dns;
    unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_errors;
};
int  rozos_netinfo(int index, struct rozos_netinfo *out);      /* -1 + errno ENODEV = নেই */
int  rozos_netconfig(unsigned ip, unsigned mask, unsigned gw, unsigned dns); /* ip 0 = DHCP */
long rozos_now_ms(void);               /* CLOCK_MONOTONIC, মিলিসেকেন্ড */

/* ধাপ ১৫ক: কার্নেল মডিউল (insmod/rmmod/lsmod) */
int  rozos_init_module(const void *image, unsigned long len, const char *name);
int  rozos_delete_module(const char *name);
long rozos_module_list(char *buf, unsigned long len, int kernel_symbols);   /* লেখা বাইট */
long rozos_usb_list(char *buf, unsigned long len);   /* ধাপ ১৫খ: lsusb */
long rozos_mounts(char *buf, unsigned long len);   /* "ডিভাইস পথ ফাইলসিস্টেম\n" সারি */

#endif
