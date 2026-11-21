/* rozOS — USB (ধাপ ১৫খ): xHCI (USB 3/2/1.1) কন্ট্রোলার, পেন ড্রাইভ, কিবোর্ড, মাউস
 *
 * xHCI কন্ট্রোলার (PCI 0C:03:30) চালু করে root port-এ লাগানো ডিভাইস চেনে:
 *   - mass storage (পেন ড্রাইভ, কার্ড রিডার): Bulk-Only + SCSI → ব্লক ডিভাইস sdb, sdc ...
 *     আর তার partition; প্রথম ext2/FAT নিজে থেকেই /mnt/usb-তে mount (দ্বিতীয়টা /mnt/usb2 ...)
 *   - HID boot keyboard / mouse → PS/2-এর মতোই কনসোল, ডেস্কটপ, /dev/kbd, /dev/mouse
 * একটা কার্নেল থ্রেড ("usbd") ২০ ms পরপর ঘটনা দেখে: কিবোর্ড/মাউসের রিপোর্ট, আর লাগানো/
 * খোলা (hot-plug)। ইন্টারাপ্ট ছাড়া (polling); hub এখনো নয় (root port-এ সরাসরি লাগাতে হবে)।
 * পেন ড্রাইভ খোলার আগে: umount /mnt/usb। */
#ifndef ROZOS_USB_H
#define ROZOS_USB_H

#include <stdint.h>
#include <stddef.h>

int usb_init(void);                              /* পাওয়া কন্ট্রোলারের সংখ্যা */
/* lsusb-এর জন্য: "port speed vid:pid class নাম\n" সারি; ফেরত লেখা বাইট */
int64_t usb_list(char *buf, size_t len);
void usb_storage_ready(void);                     /* main.c: "/" mount হয়েছে — এখন auto-mount */

#endif
