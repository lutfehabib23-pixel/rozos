/* rozOS — বিল্ড নম্বর ও তথ্য (ধাপ ১১খ)
 *
 * এই প্রতীকগুলো আসে লিংকের ঠিক আগে তৈরি হওয়া একটা ছোট C ফাইল থেকে:
 *   PC-তে (gcc):    হোস্টের Makefile → build/gen/buildinfo.c  (নম্বর: রিপোর .build-number)
 *   rozOS-এ (tcc):  /bin/mkbuildinfo → build/buildinfo.c      (নম্বর: "<PC-র নম্বর>.<ভেতরের নম্বর>")
 * তাই নম্বর বদলালে শুধু এই ফাইলটাই আবার কম্পাইল হয়। */
#ifndef ROZOS_BUILDINFO_H
#define ROZOS_BUILDINFO_H

extern const char rozos_build_id[];          /* "42" বা "42.3" */
extern const char rozos_build_compiler[];    /* "gcc" বা "tcc" */
extern const char rozos_build_where[];       /* "build host" বা "rozOS" */
extern const char rozos_build_date[];        /* "2026-11-20 16:10 UTC" */
/* "ROZOS-BUILD:<id>:<compiler>:<where>:<date>" — kinstall kernel ফাইলে এটা খুঁজে চেনে */
extern const char rozos_build_tag[];

#endif
