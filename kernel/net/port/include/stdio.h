/* rozOS — lwIP-এর জন্য <stdio.h>: কার্নেলে libc নেই, তাই কার্নেলের নিজস্ব printf।
 * (ক্রস-কম্পাইলারের সাথে কোনো libc-হেডার আসে না — এই shim-গুলো সেই ফাঁক পূরণ করে) */
#ifndef ROZOS_LWIP_STDIO_H
#define ROZOS_LWIP_STDIO_H
#include <stddef.h>
#include <rozos/printf.h>
#define snprintf ksnprintf
#define printf   kprintf
#endif
