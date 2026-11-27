/* rozOS — lwIP-র কনফিগারেশন (ধাপ ৯)
 *
 * NO_SYS = 1: lwIP-এর নিজের থ্রেড/সেমাফোর নেই। rozOS-এর কার্নেল মাঝপথে থামে না
 * (শুধু ইউজার-মোড preempt হয়) আর NIC poll হয় "netd" কার্নেল-থ্রেডে, ইন্টারাপ্টে নয় —
 * তাই netd ও syscall (socket) দুজনেই সরাসরি raw API ডাকতে পারে, lock ছাড়াই। */
#ifndef ROZOS_LWIPOPTS_H
#define ROZOS_LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0
#define LWIP_TIMERS                 1

/* মেমোরি: lwIP-এর নিজস্ব heap (স্ট্যাটিক) ও pool */
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (512 * 1024)
#define MEMP_NUM_PBUF               64
#define MEMP_NUM_RAW_PCB            8
#define MEMP_NUM_UDP_PCB            16
#define MEMP_NUM_TCP_PCB            32
#define MEMP_NUM_TCP_PCB_LISTEN     8
#define MEMP_NUM_TCP_SEG            256
#define MEMP_NUM_SYS_TIMEOUT        16
#define PBUF_POOL_SIZE              512         /* ধাপ ২৫ক: ২৫৬ → ৫১২, বড় window-এর জন্য (~৮০০ KiB) */
#define PBUF_POOL_BUFSIZE           1600

/* প্রোটোকল */
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_ACD                    0
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             2
#define IP_REASSEMBLY               1
#define IP_FRAG                     1
#define ETH_PAD_SIZE                0
#define SO_REUSE                    1

/* TCP */
#define TCP_MSS                     1460
/* ধাপ ২৫ক: window scaling — দূরের সার্ভার (GitHub) থেকে দ্রুত নামানো।
 * আগে TCP_WND ১৬ × MSS (~২৩ KiB) ছিল: গতি ≤ window / RTT (২০০ ms-এ ~১১৫ KiB/s)।
 * এখন ২৫৬ KiB (scale 3 → সর্বোচ্চ ৫১২ KiB); pbuf pool-ও তাই বড় (নিচে)। */
#define LWIP_WND_SCALE              1
#define TCP_RCV_SCALE               3
#define TCP_WND                     (256 * 1024)
/* আগে: #define TCP_WND (16 * TCP_MSS) */
#define TCP_SND_BUF                 (16 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_LISTEN_BACKLOG          1
#define LWIP_TCP_KEEPALIVE          1

/* netif — loopback: 127.0.0.1 আর নিজের ঠিকানায় পাঠানো প্যাকেট (netd netif_poll_all() চালায়) */
#define LWIP_HAVE_LOOPIF            1
#define LWIP_NETIF_LOOPBACK         1
#define LWIP_LOOPBACK_MAX_PBUFS     64
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

/* checksum সফটওয়্যারে (QEMU-র e1000 offload এখন ব্যবহার নয়) */
#define LWIP_CHKSUM_ALGORITHM       3

/* পরিসংখ্যান ও ডিবাগ বন্ধ (ifconfig-এর গণনা ড্রাইভার রাখে) */
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0

/* random: TCP পোর্ট/ISN, DHCP xid */
#include <stdint.h>
uint32_t rozos_net_rand(void);
#define LWIP_RAND()                 rozos_net_rand()

#endif
