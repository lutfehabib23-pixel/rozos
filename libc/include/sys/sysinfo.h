/* rozlibc — sys/sysinfo.h (ধাপ ১৯চ): মোট/খালি RAM, চালু থাকার সময়, প্রসেসের সংখ্যা (Linux-এর মতো) */
#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H

struct sysinfo {
    long          uptime;          /* বুট থেকে সেকেন্ড */
    unsigned long loads[3];        /* rozOS-এ 0 */
    unsigned long totalram;        /* mem_unit-এর গুণে (rozOS-এ mem_unit = 1, তাই বাইট) */
    unsigned long freeram;
    unsigned long sharedram, bufferram, totalswap, freeswap;   /* rozOS-এ 0 */
    unsigned short procs;          /* প্রসেস */
    unsigned short pad;
    unsigned long totalhigh, freehigh;
    unsigned int  mem_unit;
    char          _f[4];
};

int sysinfo(struct sysinfo *info);

#endif
