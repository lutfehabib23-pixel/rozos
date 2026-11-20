#!/usr/bin/env bash
# rozOS — rozOS-এর ভেতরে kernel বিল্ডের POSIX Makefile বানানো (ধাপ ১১)
#
#   toolchain/gen-kernel-mk.sh          > build/rozos-src/Makefile
#   toolchain/gen-kernel-mk.sh --objs   > build/rozos-src/objs.list
#
# rozOS-এ একটা প্রোগ্রামে সর্বোচ্চ ৬৪টা argument যায় (kernel-এর PROC_ARG_MAX), কিন্তু kernel-এ
# ~৮৫টা .o — তাই লিংকের সময় তালিকা ফাইলে (objs.list), tcc সেটা `@objs.list` দিয়ে পড়ে।
#
# হোস্টের Makefile এটা চালায়, তাই ফাইলের তালিকা সবসময় হোস্টের বিল্ডের সাথে মেলে।
# ফল /usr/src/rozos/Makefile — rozOS-এ `cd /usr/src/rozos && make` (pdpmake + tcc)।
# C++ অংশ (kernel/cxx) বাদ: TCC C++ পারে না (gcc বিল্ডে থাকে)।
set -euo pipefail
cd "$(dirname "$0")/.."

C_SRCS=$(find kernel -name '*.c' -not -path 'kernel/cxx/*' | sort)
S_SRCS=$(find kernel -name '*.S' | sort)
HDRS=$(find kernel \( -name '*.h' -o -name '*.inc' \) -not -path 'kernel/cxx/*' | sort)

obj() { echo "build/${1%.*}.o"; }

if [ "${1:-}" = "--objs" ]; then
    for f in $C_SRCS $S_SRCS; do obj "$f"; done
    exit 0
fi

cat <<'HDR'
# rozOS kernel — rozOS-এর ভেতরে বিল্ড (pdpmake + tcc)। এই ফাইল হোস্টে
# toolchain/gen-kernel-mk.sh দিয়ে তৈরি — হাতে বদলাবেন না, হোস্টে নতুন করে বানান।
#
#   make            build/rozos.elf বানানো (প্রথমবার ~১-২ মিনিট)
#   make clean      build/ মোছা
#
# প্রতিটি .o সব হেডারের ওপর নির্ভর করে — কোনো .h বদলালে সব আবার কম্পাইল হয় (নিরাপদ পথ)।
# পরীক্ষা: help kernel-build

CC      = tcc
TCCINC  = /usr/lib/tcc/include
CFLAGS  = -nostdinc -std=c99 -Ikernel/include -Ikernel -I$(TCCINC) -Ikernel/include/tcc
NETINC  = -Ikernel/net/port/include -Ikernel/net/lwip/include
ASFLAGS = -nostdinc -D__ASSEMBLY__ -Ikernel/include -Ikernel -I$(TCCINC)
# kernel = শেষ 2 GiB (0xffffffff80000000); TCC আলাদা R / RX / RW PT_LOAD বানায়
LDFLAGS = -nostdlib -static -Wl,-Ttext=0xffffffff80000000
# TCC-র variadic সহায়ক (__va_arg) — /usr/lib/tcc/libtcc1.a gcc-বিল্ড (red zone ব্যবহার
# করতে পারে, kernel-এ ইন্টারাপ্ট তা নষ্ট করবে), তাই সোর্স থেকে tcc দিয়ে আলাদা কম্পাইল
VALIST  = /usr/src/tcc/lib/va_list.c
LIBS    = build/va_list.o

HDR

printf 'OBJS ='
for f in $C_SRCS $S_SRCS; do printf ' \\\n\t%s' "$(obj "$f")"; done
printf '\n\n'
printf 'HDRS ='
for f in $HDRS; do printf ' \\\n\t%s' "$f"; done
printf '\n\n'

cat <<'RULES'
all: build/rozos.elf

# ধাপ ১১খ: প্রতিটি লিংকে নতুন বিল্ড নম্বর (mkbuildinfo; help kinstall)
build/rozos.elf: $(OBJS) $(LIBS) objs.list
	mkbuildinfo . > build/buildinfo.c
	$(CC) $(CFLAGS) -c build/buildinfo.c -o build/buildinfo.o
	$(CC) $(LDFLAGS) -o build/rozos.elf @objs.list build/buildinfo.o $(LIBS)
	@echo "kernel ready: /usr/src/rozos/build/rozos.elf  (install: kinstall, then reboot)"

clean:
	rm -r build

$(OBJS): $(HDRS)

build/va_list.o: $(VALIST)
	@mkdir -p build
	$(CC) -nostdinc -I$(TCCINC) -c $(VALIST) -o build/va_list.o

RULES

for f in $C_SRCS; do
    o=$(obj "$f")
    extra=""
    case "$f" in kernel/net/*|kernel/syscall/sys_net.c) extra=' $(NETINC)';; esac
    printf '%s: %s\n\t@mkdir -p %s\n\t$(CC) $(CFLAGS)%s -c %s -o %s\n\n' "$o" "$f" "$(dirname "$o")" "$extra" "$f" "$o"
done
for f in $S_SRCS; do
    o=$(obj "$f")
    printf '%s: %s\n\t@mkdir -p %s\n\t$(CC) $(ASFLAGS) -c %s -o %s\n\n' "$o" "$f" "$(dirname "$o")" "$f" "$o"
done
