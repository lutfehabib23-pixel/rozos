# rozOS — টপ-লেভেল Makefile
#
#   make            কার্নেল বিল্ড (build/rozos.elf)
#   make initrd     sysroot/ থেকে build/initrd.tar (USTAR)
#   make iso        বুটযোগ্য ISO (rozos.iso) — BIOS ও UEFI দুটোতেই চলে
#   make disk       sysroot/ থেকে ext2 ডিস্ক ইমেজ (rozos-disk.img) — শুধু না থাকলে বানায়
#   make disk-reset ডিস্ক ইমেজ মুছে sysroot/ থেকে নতুন করে (ভেতরের সব পরিবর্তন হারাবে!)
#   make disk-check হোস্টের e2fsck দিয়ে ডিস্ক ইমেজ যাচাই (কিছু বদলায় না)
#   make libc       rozlibc (ধাপ ৭): build/libc/libc.a + crt0.o (+ crti/crtn, ধাপ ৮)
#   ./toolchain/get-tcc.sh   TCC সোর্স আনা (ধাপ ৮) — তারপর make userland-এ /bin/tcc
#   make userland   ইউজার প্রোগ্রাম (ধাপ ৬-৭): build/userland/bin/* (init, rozsh, edit, ls ...)
#   make disk-update ডিস্কের /bin, /usr/lib, /usr/include-এ শুধু নতুন বিল্ড বসায় (বাকি ডেটা অক্ষত)
#   make self-iso    ধাপ ১১: rozOS-এর ভেতরে বানানো kernel (ডিস্ক থেকে) দিয়ে rozos-self.iso
#   make run-self    সেই ISO দিয়ে boot (serial টার্মিনালে)
#   make disk-migrate ধাপ ১১খ: পুরোনো ডিস্ক → GPT (boot FAT32 + ext2 root), ফাইল অক্ষত
#   make run-iso     ISO থেকে boot (rescue); make run / run-serial এখন ডিস্ক থেকে
#                   — run টার্গেটগুলো প্রোগ্রাম বদলালে এটা নিজেই চালায়
#   make run        QEMU-তে BIOS মোডে চালানো (জানালা + serial টার্মিনালে)
#   make run-uefi   QEMU-তে UEFI (OVMF) মোডে চালানো
#   make run-serial জানালা ছাড়া, শুধু serial আউটপুট টার্মিনালে
#   make debug      QEMU থামিয়ে রাখে, GDB-র জন্য অপেক্ষা করে (localhost:1234)
#   make gdb        অন্য টার্মিনাল থেকে GDB দিয়ে যুক্ত হওয়া
#   make clean      বিল্ডের সব আউটপুট মোছা

include config.mk

BUILD      := build
KERNEL     := $(BUILD)/rozos.elf
ISO        := rozos.iso
ISO_ROOT   := $(BUILD)/iso_root
LIMINE_DIR := limine
SYSROOT    := sysroot
INITRD     := $(BUILD)/initrd.tar
SYSROOT_FILES := $(shell find $(SYSROOT) -type f -o -type d 2>/dev/null)
DISK       := rozos-disk.img
DISK_SIZE  ?= 64M          # ধাপ ৫-১১ক-এর পুরোনো ডিস্ক (partition ছাড়া) — শুধু তথ্য
# ধাপ ১১খ: GPT ডিস্ক — sda1 FAT32 boot (ESP_MIB), sda2 ext2 root (বাকিটা)
# ধাপ ১২ক: 512 MiB (আগের ছোট ডিস্ক disk-update-এ ডেটা অক্ষত রেখে বড় হয় — toolchain/disk-grow.sh)
DISK_MIB   ?= 512
ESP_MIB    ?= 64
ESP_STAGE  := $(BUILD)/esp-stage
DISK_STAGE := $(BUILD)/disk-root
# q35-এ index=0 মানে AHCI-র প্রথম পোর্ট (CD-ROM থাকে index 2-এ)
QEMU_DISK  = -drive file=$(DISK),format=raw,index=0,media=disk
# ধাপ ১৫ক: make run NVME=1 — ৬৪ MiB-এর একটা NVMe SSD (nvme-disk.img, FAT32; প্রথমবার বানানো হয়)
NVME       ?= 0
NVME_IMG   ?= nvme-disk.img
ifeq ($(NVME),1)
QEMU_DISK += -drive file=$(NVME_IMG),format=raw,if=none,id=nvme0 -device nvme,serial=rozos-nvme,drive=nvme0
NVME_DEP   := $(NVME_IMG)
endif
# ধাপ ১৫খ: make run USB=1 — xHCI কন্ট্রোলার + পেন ড্রাইভ (usb-disk.img, ৬৪ MiB, MBR + FAT32)
# + USB কিবোর্ড ও মাউস। আসল পেন ড্রাইভ: make run USB=1 USB_DEV=/dev/sdX (WSL-এ usbipd দিয়ে
# জোড়া ডিস্ক; help usb)
USB        ?= 0
USB_IMG    ?= usb-disk.img
USB_DEV    ?=
ifeq ($(USB),1)
QEMU_DISK += -device qemu-xhci,id=xhci -drive file=$(if $(USB_DEV),$(USB_DEV),$(USB_IMG)),format=raw,if=none,id=usbstick \
             -device usb-storage,bus=xhci.0,drive=usbstick -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0
USB_DEP    := $(if $(USB_DEV),,$(USB_IMG))
endif

# ---------- rozlibc ও ইউজার প্রোগ্রাম (ধাপ ৬-৭) ----------
# rozlibc (libc/): নিজস্ব ছোট C লাইব্রেরি → build/libc/libc.a + crt0.o। হোস্টের কোনো
# হেডার যেন না ঢোকে তাই -nostdinc; কম্পাইলারের নিজের stddef/stdarg/stdint (-isystem)।
# প্রোগ্রাম: userland/<dir>/<নাম>.c → /bin/<নাম> (স্ট্যাটিক, non-PIE, 0x400000)।
USER_AR      := $(CROSS)ar
USER_STRIP   := $(CROSS)strip
CC_INCLUDE   := $(shell $(CC) -print-file-name=include)
LIBC_BUILD   := $(BUILD)/libc
LIBC_OBJS    := $(patsubst libc/src/%.c,$(LIBC_BUILD)/%.o,$(wildcard libc/src/*.c)) \
                $(LIBC_BUILD)/setjmp.o
LIBC_A       := $(LIBC_BUILD)/libc.a
LIBC_M       := $(LIBC_BUILD)/libm.a      # math আসলে libc-তেই; -lm চাওয়া প্রোগ্রামের জন্য
LIBC_CRT0    := $(LIBC_BUILD)/crt0.o
LIBC_CRTS    := $(LIBC_BUILD)/crti.o $(LIBC_BUILD)/crtn.o $(LIBC_M)   # TCC-র লিংকের জন্য
USER_BUILD   := $(BUILD)/userland
USER_DIRS    := init shell editor coreutils bin tests net gfx
USER_SRCS    := $(foreach d,$(USER_DIRS),$(wildcard userland/$(d)/*.c))
USER_PROGS   := $(sort $(notdir $(basename $(USER_SRCS))))
USER_BINS    := $(addprefix $(USER_BUILD)/bin/,$(USER_PROGS))
# rozOS-এ যায় debug-তথ্য ছাড়া কপি (৪-৫ গুণ ছোট); build/userland/bin-এরগুলো gdb-র জন্য থাকে
USER_STRIPPED := $(addprefix $(USER_BUILD)/stripped/,$(USER_PROGS))
USER_CFLAGS  := -std=c99 $(OPT) -g -pipe -Wall -Wextra -m64 -march=x86-64 \
                -ffreestanding -nostdinc -isystem $(CC_INCLUDE) -Ilibc/include \
                -fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables \
                -Werror=implicit-function-declaration -Werror=int-conversion \
                -Werror=incompatible-pointer-types
LIBC_CFLAGS  := $(USER_CFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns
USER_LDFLAGS := -nostdlib -static -no-pie -Wl,-z,max-page-size=0x1000 \
                -Wl,--build-id=none -Wl,-Ttext-segment=0x400000
vpath %.c $(addprefix userland/,$(USER_DIRS))

# ---------- BearSSL (ধাপ ১০: TLS) ----------
# সোর্স: ./toolchain/get-bearssl.sh → ports/bearssl/src। না থাকলে https ছাড়া বিল্ড হয়।
BEARSSL_SRC  := ports/bearssl/src
BEARSSL_BUILD := $(BUILD)/bearssl
HAVE_BEARSSL := $(if $(wildcard $(BEARSSL_SRC)/inc/bearssl.h),1,)
BEARSSL_OBJS := $(patsubst $(BEARSSL_SRC)/src/%.c,$(BEARSSL_BUILD)/%.o,$(shell find $(BEARSSL_SRC)/src -name '*.c' 2>/dev/null))
BEARSSL_A    := $(BEARSSL_BUILD)/libbearssl.a
BEARSSL_CFLAGS := $(USER_CFLAGS) -I$(BEARSSL_SRC)/inc -I$(BEARSSL_SRC)/src \
                  -DBR_USE_URANDOM=1 -DBR_USE_UNIX_TIME=1 -w

# ---------- rozhttp (ধাপ ১০): http/https ক্লায়েন্ট লাইব্রেরি — http-get ও rozpkg ব্যবহার করে ----------
ROZHTTP_SRCS := userland/lib/rozhttp.c    # শুধু এটা — পুরোনো ধাপ ৬-এর userland/lib/ulib.c (থাকলে) বাদ
ROZHTTP_OBJS := $(patsubst userland/lib/%.c,$(USER_BUILD)/lib/%.o,$(ROZHTTP_SRCS))
ROZHTTP_A    := $(USER_BUILD)/lib/librozhttp.a
# ধাপ ১৩: librozgfx (গ্রাফিক্স; rozgfx_ttf.c-তে stb_truetype)
ROZGFX_SRCS  := userland/lib/rozgfx.c userland/lib/rozgfx_ttf.c userland/lib/rozgfx_img.c
ROZGFX_OBJS  := $(patsubst userland/lib/%.c,$(USER_BUILD)/lib/%.o,$(ROZGFX_SRCS))
ROZGFX_A     := $(USER_BUILD)/lib/librozgfx.a
# ধাপ ১৪: rozui (3D UI) — rozgfx-এর ওপর
ROZUI_SRCS   := userland/lib/rozui.c
ROZUI_OBJS   := $(patsubst userland/lib/%.c,$(USER_BUILD)/lib/%.o,$(ROZUI_SRCS))
ROZUI_A      := $(USER_BUILD)/lib/librozui.a
USER_LIBS    := $(ROZHTTP_A) $(ROZUI_A) $(ROZGFX_A)
ifeq ($(HAVE_BEARSSL),1)
  USER_CFLAGS += -DROZOS_HAVE_BEARSSL -I$(BEARSSL_SRC)/inc
  USER_LIBS += $(BEARSSL_A)
endif

# ধাপ ১০: TLS-এর বিশ্বস্ত CA — হোস্টের Mozilla bundle (Debian/Ubuntu: ca-certificates প্যাকেজ)
# → rozOS-এর /etc/ssl/certs.pem। CA_EXTRA=file.pem দিলে সেটাও যোগ হয় (নিজের/পরীক্ষার CA)।
CA_BUNDLE    ?= /etc/ssl/certs/ca-certificates.crt
CA_EXTRA     ?=
CA_PEM       := $(BUILD)/ssl/certs.pem
# TCC দিয়ে rozOS-এর ভেতরে https প্রোগ্রাম লেখার জন্য: /usr/lib + /usr/include-এ বসে
# (debug তথ্য ছাড়া কপি — libbearssl.a 4.5 MB থেকে অনেক ছোট হয়)
DEV_LIBS     := $(BUILD)/devlib/librozhttp.a $(BUILD)/devlib/librozgfx.a $(BUILD)/devlib/librozui.a $(if $(HAVE_BEARSSL),$(BUILD)/devlib/libbearssl.a)
DEV_HEADERS  := userland/lib/rozhttp.h userland/lib/rozgfx.h userland/lib/rozui.h userland/lib/rozwm_proto.h $(if $(HAVE_BEARSSL),$(wildcard $(BEARSSL_SRC)/inc/*.h))

# ধাপ ১১: kernel-এর সোর্স rozOS-এর /usr/src/rozos-এ — ভেতরে `make` (pdpmake + tcc) দিয়ে বিল্ড।
# সেখানকার Makefile হোস্টে toolchain/gen-kernel-mk.sh বানায় (ফাইলের তালিকা এখানের সাথে মেলে)।
KSRC_FILES   := $(shell find kernel -type f | sort)
KSRC_DIRS    := $(shell find kernel -type d | sort)
KSRC_MK      := $(BUILD)/rozos-src/Makefile
KSRC_OBJS    := $(BUILD)/rozos-src/objs.list
# rozOS-এর ভেতরে বানানো kernel (ডিস্ক থেকে বের করে) দিয়ে আলাদা ISO: make self-iso / run-self
SELF_KERNEL  := $(BUILD)/self/rozos.elf
SELF_ISO     := rozos-self.iso

# সাহায্যের পাতা (ধাপ ৮): docs/help/*.txt → /usr/share/help (rozhelp / shell-এর help)
HELP_FILES   := $(wildcard docs/help/*.txt)

# ---------- TCC (ধাপ ৮) ----------
# সোর্স: ./toolchain/get-tcc.sh → ports/tcc/src (নির্দিষ্ট mob commit + rozos.patch)।
# না থাকলে বাকি সব বিল্ড হয়, শুধু /bin/tcc বাদ যায় (একটা সতর্কবার্তা সহ)।
TCC_SRC      := ports/tcc/src
TCC_BUILD    := $(BUILD)/tcc
HAVE_TCC     := $(if $(wildcard $(TCC_SRC)/tcc.c),1,)
TCC_LIB1_OBJS := $(addprefix $(TCC_BUILD)/lib/,libtcc1.o stdatomic.o atomic.o builtin.o \
                 alloca.o alloca-bt.o va_list.o dsohandle.o)
TCC_LIB1     := $(TCC_BUILD)/libtcc1.a
TCC_RUNMAIN  := $(TCC_BUILD)/lib/runmain.o     # `tcc -run`-এর জন্য ({B}/runmain.o)
TCC_CFLAGS   := $(USER_CFLAGS) -Iports/tcc -I$(TCC_SRC) -w
# rozOS-এর /usr/src/tcc-তে যায় (ভেতরে TCC দিয়ে TCC আবার বানানোর জন্য)
TCC_SRC_FILES := $(wildcard $(TCC_SRC)/*.c $(TCC_SRC)/*.h $(TCC_SRC)/*.def $(TCC_SRC)/COPYING)
ifeq ($(HAVE_TCC),1)
  USER_PROGS += tcc
  USER_BINS += $(USER_BUILD)/bin/tcc
  USER_STRIPPED += $(USER_BUILD)/stripped/tcc
  TCC_EXTRA := $(TCC_LIB1) $(TCC_RUNMAIN)
endif
# ---------- pdpmake = /bin/make (ধাপ ১১) ----------
# সোর্স: ./toolchain/get-pdpmake.sh → ports/pdpmake/src। না থাকলে /bin/make বাদ।
PDPMAKE_SRC   := ports/pdpmake/src
PDPMAKE_BUILD := $(BUILD)/pdpmake
HAVE_PDPMAKE  := $(if $(wildcard $(PDPMAKE_SRC)/make.h),1,)
PDPMAKE_OBJS  := $(patsubst $(PDPMAKE_SRC)/%.c,$(PDPMAKE_BUILD)/%.o,$(wildcard $(PDPMAKE_SRC)/*.c))
ifeq ($(HAVE_PDPMAKE),1)
  USER_PROGS += make
  USER_BINS += $(USER_BUILD)/bin/make
  USER_STRIPPED += $(USER_BUILD)/stripped/make
endif
# /bin/sh = rozsh-এর কপি (system(), make-এর রেসিপি `sh -c` দিয়ে চলে)
USER_PROGS += sh
USER_BINS += $(USER_BUILD)/bin/sh
USER_STRIPPED += $(USER_BUILD)/stripped/sh
# ধাপ ১২: /bin/menu = rozmenu-এর কপি (shell-এ `menu`)
USER_PROGS += menu
USER_BINS += $(USER_BUILD)/bin/menu
USER_STRIPPED += $(USER_BUILD)/stripped/menu
# ধাপ ১২: মেনুর এন্ট্রি (sysroot/usr/share/apps/*.app) — disk-update-এ হালনাগাদ হয়
APP_FILES    := $(wildcard sysroot/usr/share/apps/*.app)
# ধাপ ১২ক: edit --new-এর টেমপ্লেট (sysroot/usr/share/templates/*)
TEMPLATE_FILES := $(wildcard sysroot/usr/share/templates/*)
# ধাপ ১৩: ফন্ট (PSF কনসোল-ফন্ট, TrueType) আর গ্রাফিক্স ডেমোর ছবি
FONT_FILES   := $(wildcard sysroot/usr/share/fonts/*)
IMAGE_FILES  := $(wildcard sysroot/usr/share/images/*)
# ধাপ ১৪খ: আইকনের ছবি (/usr/share/icons/<নাম>.png — না থাকলে আঁকা চিহ্ন)
ICON_FILES   := $(wildcard sysroot/usr/share/icons/*)
# ধাপ ১৫ক: উদাহরণ কার্নেল মডিউল (modules/*.c → /lib/modules/*.ko, সোর্স /usr/src/modules)
MOD_SRCS     := $(wildcard modules/*.c)
MOD_KOS      := $(patsubst modules/%.c,$(BUILD)/modules/%.ko,$(MOD_SRCS))
# ধাপ ১৩: গ্রাফিক্স ডেমোর সোর্স /usr/src/gfx-এ (নিজে বদলে tcc দিয়ে চালানোর জন্য)
GFX_SRC_FILES := $(wildcard userland/gfx/*.c)

# initrd ও ডিস্কের জন্য sysroot + বিল্ড করা প্রোগ্রাম একসাথে
ROOT_STAGE   := $(BUILD)/root-stage
DISK_BIN_STAMP := $(BUILD)/.disk-bin-stamp

# ---------- সোর্স ফাইল খোঁজা ----------
# kernel/cxx/ বাদে সব .c আর .S; C++ শুধু ENABLE_KCXX=1 হলে
C_SRCS   := $(shell find kernel -name '*.c' -not -path 'kernel/cxx/*' | sort)
S_SRCS   := $(shell find kernel -name '*.S' | sort)
ifeq ($(ENABLE_KCXX),1)
  CXX_SRCS := $(shell find kernel/cxx -name '*.cpp' | sort)
else
  CXX_SRCS :=
endif

OBJS := $(patsubst %,$(BUILD)/%.o,$(C_SRCS) $(S_SRCS) $(CXX_SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all kernel libc userland tcc-check disk-update initrd iso disk disk-reset disk-check run run-uefi run-serial debug gdb clean check-tools info self-iso run-self disk-migrate run-iso run-iso-serial run-uefi-iso disk-grow

all: kernel userland $(MOD_KOS)

kernel: $(KERNEL)

libc: $(LIBC_A) $(LIBC_CRT0) $(LIBC_CRTS)

userland: $(USER_BINS) $(LIBC_CRTS) $(TCC_EXTRA) tcc-check

# ---------- কম্পাইল ----------
$(BUILD)/%.c.o: %.c config.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD)/%.S.o: %.S config.mk
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	@$(CC) $(KERNEL_ASFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: %.cpp config.mk
	@mkdir -p $(dir $@)
	@echo "  CXX     $<"
	@$(CXX) $(KERNEL_CXXFLAGS) -c $< -o $@

# ---------- লিংক ----------
# ধাপ ১১খ: প্রতিটি লিংকে বিল্ড নম্বর +১ (রিপোর .build-number — make clean-এও থাকে),
# তারপর build/gen/buildinfo.c বানিয়ে কম্পাইল (rozos/buildinfo.h দেখুন)
BUILD_NUMBER_FILE := .build-number
BUILDINFO_C := $(BUILD)/gen/buildinfo.c
BUILDINFO_O := $(BUILD)/gen/buildinfo.o

$(KERNEL): $(OBJS) kernel/linker.ld
	@mkdir -p $(dir $@) $(BUILD)/gen
	@n=$$(cat $(BUILD_NUMBER_FILE) 2>/dev/null || echo 0); n=$$((n + 1)); echo $$n > $(BUILD_NUMBER_FILE); \
	 d=$$(date -u '+%Y-%m-%d %H:%M UTC'); \
	 printf '/* make তৈরি করে — বদলাবেন না */\nconst char rozos_build_id[] = "%s";\nconst char rozos_build_compiler[] = "gcc";\nconst char rozos_build_where[] = "build host";\nconst char rozos_build_date[] = "%s";\nconst char rozos_build_tag[] = "ROZOS-BUILD:%s:gcc:build host:%s";\n' \
	   "$$n" "$$d" "$$n" "$$d" > $(BUILDINFO_C); \
	 echo "  BUILD   #$$n"
	@$(CC) $(KERNEL_CFLAGS) -c $(BUILDINFO_C) -o $(BUILDINFO_O)
	@echo "  LD      $@"
	@$(CC) $(KERNEL_LDFLAGS) $(OBJS) $(BUILDINFO_O) $(KERNEL_LIBS) -o $@

-include $(DEPS)

# ধাপ ৯: lwIP (kernel/net/lwip) ও তার port — নিজস্ব include পথ, আর lwIP-এর
# নিজের কোডে warning বন্ধ (আমরা ওটা বদলাই না)
$(BUILD)/kernel/net/%: KERNEL_CFLAGS += -Ikernel/net/port/include -Ikernel/net/lwip/include
$(BUILD)/kernel/net/lwip/%: KERNEL_CFLAGS += -w -Werror=implicit-function-declaration \
    -Werror=int-conversion -Werror=incompatible-pointer-types   # GCC 14+ এগুলো সবসময় error
$(BUILD)/kernel/syscall/sys_net.c.o: KERNEL_CFLAGS += -Ikernel/net/port/include -Ikernel/net/lwip/include
-include $(wildcard $(USER_BUILD)/*/*.d $(LIBC_BUILD)/*.d)

# ---------- rozlibc বিল্ড ----------
$(LIBC_BUILD)/%.o: libc/src/%.c config.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $< (libc)"
	@$(CC) $(LIBC_CFLAGS) -MMD -MP -c $< -o $@

$(LIBC_BUILD)/%.o: libc/src/%.S config.mk
	@mkdir -p $(dir $@)
	@echo "  AS      $< (libc)"
	@$(CC) $(LIBC_CFLAGS) -c $< -o $@

$(LIBC_A): $(LIBC_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(USER_AR) rcs $@ $(LIBC_OBJS)

$(LIBC_M): $(LIBC_BUILD)/math.o
	@rm -f $@
	@$(USER_AR) rcs $@ $<

# ---------- ইউজার প্রোগ্রাম বিল্ড ----------
$(USER_BUILD)/obj/%.o: %.c config.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $< (user)"
	@$(CC) $(USER_CFLAGS) -Iuserland/lib -MMD -MP -c $< -o $@

$(USER_BUILD)/bin/%: $(USER_BUILD)/obj/%.o $(LIBC_A) $(LIBC_CRT0) $(USER_LIBS)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CC) $(USER_LDFLAGS) $(LIBC_CRT0) $< $(USER_LIBS) $(LIBC_A) -lgcc -o $@

# ---------- rozhttp ও BearSSL বিল্ড ----------
$(USER_BUILD)/lib/%.o: userland/lib/%.c config.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $< (lib)"
	@$(CC) $(USER_CFLAGS) $(LIB_EXTRA_CFLAGS) -Iuserland/lib -MMD -MP -c $< -o $@

# stb_truetype (বাইরের কোড) — সতর্কবার্তা বন্ধ
$(USER_BUILD)/lib/rozgfx_ttf.o: LIB_EXTRA_CFLAGS := -w
$(USER_BUILD)/lib/rozgfx_img.o: LIB_EXTRA_CFLAGS := -w

$(ROZGFX_A): $(ROZGFX_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(USER_AR) rcs $@ $(ROZGFX_OBJS)

$(ROZUI_A): $(ROZUI_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(USER_AR) rcs $@ $(ROZUI_OBJS)

$(ROZHTTP_A): $(ROZHTTP_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(USER_AR) rcs $@ $(ROZHTTP_OBJS)

$(BEARSSL_BUILD)/%.o: $(BEARSSL_SRC)/src/%.c config.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $< (bearssl)"
	@$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(BEARSSL_A): $(BEARSSL_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(USER_AR) rcs $@ $(BEARSSL_OBJS)

$(USER_BUILD)/stripped/%: $(USER_BUILD)/bin/%
	@mkdir -p $(dir $@)
	@$(USER_STRIP) -o $@ $<

# ---------- pdpmake ও sh ----------
$(PDPMAKE_BUILD)/%.o: $(PDPMAKE_SRC)/%.c $(PDPMAKE_SRC)/make.h config.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $< (pdpmake)"
	@$(CC) $(USER_CFLAGS) -w -c $< -o $@

$(USER_BUILD)/bin/make: $(PDPMAKE_OBJS) $(LIBC_A) $(LIBC_CRT0)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CC) $(USER_LDFLAGS) $(LIBC_CRT0) $(PDPMAKE_OBJS) $(LIBC_A) -lgcc -o $@

$(USER_BUILD)/bin/sh: $(USER_BUILD)/bin/rozsh
	@cp $< $@

$(USER_BUILD)/bin/menu: $(USER_BUILD)/bin/rozmenu
	@cp $< $@

# ---------- TCC বিল্ড (হোস্টের ক্রস-কম্পাইলার দিয়ে, rozlibc-র সাথে) ----------
$(TCC_BUILD)/tcc.o: $(TCC_SRC)/tcc.c ports/tcc/config.h $(wildcard $(TCC_SRC)/*.c $(TCC_SRC)/*.h)
	@mkdir -p $(dir $@)
	@echo "  CC      $< (tcc)"
	@$(CC) $(TCC_CFLAGS) -c $< -o $@

$(USER_BUILD)/bin/tcc: $(TCC_BUILD)/tcc.o $(LIBC_A) $(LIBC_CRT0)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CC) $(USER_LDFLAGS) $(LIBC_CRT0) $< $(LIBC_A) -lgcc -o $@

$(TCC_BUILD)/lib/%.o: $(TCC_SRC)/lib/%.c config.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $< (libtcc1)"
	@$(CC) $(TCC_CFLAGS) -fno-omit-frame-pointer -c $< -o $@

$(TCC_BUILD)/lib/%.o: $(TCC_SRC)/lib/%.S config.mk
	@mkdir -p $(dir $@)
	@echo "  AS      $< (libtcc1)"
	@$(CC) $(TCC_CFLAGS) -c $< -o $@

$(TCC_LIB1): $(TCC_LIB1_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(USER_AR) rcs $@ $(TCC_LIB1_OBJS)

tcc-check:
	@test -n "$(HAVE_TCC)" || echo "  NOTE    TCC সোর্স নেই — /bin/tcc বাদ। আনতে: ./toolchain/get-tcc.sh"
	@test -n "$(HAVE_PDPMAKE)" || echo "  NOTE    pdpmake সোর্স নেই — /bin/make বাদ। আনতে: ./toolchain/get-pdpmake.sh"
	@test -n "$(HAVE_BEARSSL)" || echo "  NOTE    BearSSL সোর্স নেই — https ও rozpkg চলবে না। আনতে: ./toolchain/get-bearssl.sh"

# মধ্যবর্তী .o ফাইল make যেন মুছে না ফেলে (নইলে প্রতিবার আবার বিল্ড)
.SECONDARY: $(PDPMAKE_OBJS) $(BEARSSL_OBJS) $(ROZHTTP_OBJS) $(ROZGFX_OBJS) $(ROZUI_OBJS) $(LIBC_OBJS) $(LIBC_CRT0) $(LIBC_CRTS) $(TCC_LIB1_OBJS) $(TCC_RUNMAIN) $(TCC_BUILD)/tcc.o $(patsubst %,$(USER_BUILD)/obj/%.o,$(USER_PROGS)) $(USER_BINS)

# sysroot + /bin-এর প্রোগ্রাম + rozlibc (/usr/include, /usr/lib — ধাপ ৮-এর TCC-র জন্য)
$(BUILD)/modules/%.ko: modules/%.c kernel/include/rozos/module.h
	@mkdir -p $(dir $@)
	@echo "  MOD     $@"
	@$(CC) $(filter-out -ffunction-sections -fdata-sections -MMD -MP -g,$(KERNEL_CFLAGS)) -c $< -o $@

define stage_root
	@rm -rf $(1) && mkdir -p $(1)
	@tar -C $(SYSROOT) --exclude=.keep -cf - . | tar -C $(1) -xf -
	@mkdir -p $(1)/bin $(1)/tmp $(1)/dev $(1)/usr/include $(1)/usr/lib
	@cp $(USER_STRIPPED) $(1)/bin/
	@cp -r libc/include/. $(1)/usr/include/
	@cp $(LIBC_A) $(LIBC_CRT0) $(LIBC_CRTS) $(1)/usr/lib/
	@cp $(LIBC_CRT0) $(1)/usr/lib/crt1.o
	@mkdir -p $(1)/usr/share/help && cp $(HELP_FILES) $(1)/usr/share/help/
	@mkdir -p $(1)/etc/ssl $(1)/var/cache/rozpkg $(1)/var/lib/rozpkg/installed && cp $(CA_PEM) $(1)/etc/ssl/certs.pem
	@cp $(DEV_LIBS) $(1)/usr/lib/ && cp $(DEV_HEADERS) $(1)/usr/include/
	@mkdir -p $(1)/usr/src/gfx && cp $(GFX_SRC_FILES) $(1)/usr/src/gfx/
	@mkdir -p $(1)/lib/modules $(1)/usr/src/modules && cp $(MOD_KOS) $(1)/lib/modules/ && cp $(MOD_SRCS) $(1)/usr/src/modules/
	@mkdir -p $(1)/usr/src/rozos && cp -r kernel $(1)/usr/src/rozos/ && cp $(KSRC_MK) $(1)/usr/src/rozos/Makefile && cp $(KSRC_OBJS) $(1)/usr/src/rozos/objs.list && (cat $(BUILD_NUMBER_FILE) 2>/dev/null || echo 0) > $(1)/usr/src/rozos/.build-base
	$(if $(HAVE_TCC),@mkdir -p $(1)/usr/lib/tcc/include && cp $(TCC_SRC)/include/*.h $(1)/usr/lib/tcc/include/ && cp $(TCC_LIB1) $(TCC_RUNMAIN) $(1)/usr/lib/tcc/)
	$(if $(HAVE_TCC),@mkdir -p $(1)/usr/src/tcc && cp $(TCC_SRC_FILES) ports/tcc/config.h ports/tcc/build-in-rozos.sh $(1)/usr/src/tcc/ && cp -r $(TCC_SRC)/include $(TCC_SRC)/lib $(1)/usr/src/tcc/)
endef

$(BUILD)/devlib/librozhttp.a: $(ROZHTTP_A)
	@mkdir -p $(dir $@)
	@strip --strip-debug -o $@ $<

$(BUILD)/devlib/librozgfx.a: $(ROZGFX_A)
	@mkdir -p $(dir $@)
	@strip --strip-debug -o $@ $<

$(BUILD)/devlib/librozui.a: $(ROZUI_A)
	@mkdir -p $(dir $@)
	@strip --strip-debug -o $@ $<

$(BUILD)/devlib/libbearssl.a: $(BEARSSL_A)
	@mkdir -p $(dir $@)
	@echo "  STRIP   $@"
	@strip --strip-debug -o $@ $<

$(KSRC_MK): toolchain/gen-kernel-mk.sh $(KSRC_FILES)
	@mkdir -p $(dir $@)
	@echo "  GEN     $@ (rozOS-এর ভেতরের kernel Makefile)"
	@./toolchain/gen-kernel-mk.sh > $@
	@./toolchain/gen-kernel-mk.sh --objs > $(KSRC_OBJS)

$(CA_PEM): $(wildcard $(CA_BUNDLE)) $(CA_EXTRA)
	@mkdir -p $(dir $@)
	@if [ -f "$(CA_BUNDLE)" ]; then cat $(CA_BUNDLE) $(CA_EXTRA) > $@; \
	  echo "  CA      $@ ($$(grep -c 'BEGIN CERTIFICATE' $@) trusted certificates)"; \
	else cat /dev/null $(CA_EXTRA) > $@; \
	  echo "  NOTE    $(CA_BUNDLE) নেই — sudo apt install ca-certificates (নইলে https যাচাই ব্যর্থ হবে)"; fi

# ---------- initrd ----------
# USTAR ফরম্যাট (কার্নেলের initrd.c এটাই পড়ে); মালিক root, যাতে হোস্টের ইউজার নাম না ঢোকে
initrd: $(INITRD)

$(INITRD): $(SYSROOT_FILES) $(USER_STRIPPED) $(LIBC_A) $(LIBC_CRTS) $(TCC_EXTRA) $(HELP_FILES) $(CA_PEM) $(DEV_LIBS) $(DEV_HEADERS) $(KSRC_MK) $(KSRC_FILES) $(MOD_KOS)
	@mkdir -p $(dir $@)
	@echo "  TAR     $@"
	$(call stage_root,$(ROOT_STAGE))
	@tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name \
	    -cf $@ -C $(ROOT_STAGE) .

# ---------- ডিস্ক ইমেজ (ধাপ ৫) ----------
# ইচ্ছাকৃতভাবে sysroot/-এর ওপর নির্ভরশীল নয়: একবার বানানোর পর rozOS-এর ভেতরে লেখা
# ফাইল যেন পরের make-এ মুছে না যায়। sysroot বদলালে `make disk-reset` চালাতে হয়।
disk: $(DISK)

# প্রোগ্রামগুলো order-only: প্রোগ্রাম বদলালে পুরো ডিস্ক নতুন হয় না (disk-update দেখুন)
# ধাপ ১১খ: GPT ডিস্ক — toolchain/mkdisk.sh (sgdisk, mkfs.fat, mtools লাগে)
$(DISK): | $(USER_STRIPPED) $(LIBC_A) $(LIBC_CRTS) $(TCC_EXTRA) $(CA_PEM) $(DEV_LIBS) $(KSRC_MK) $(KERNEL) $(INITRD) $(LIMINE_FILES) $(MOD_KOS)
	@command -v mke2fs >/dev/null || { echo "mke2fs নেই — sudo apt install e2fsprogs"; exit 1; }
	@echo "  MKDISK  $@ ($(DISK_MIB) MiB: FAT32 boot $(ESP_MIB) MiB + ext2 root, from $(SYSROOT)/ + build/userland/bin)"
	$(call stage_root,$(DISK_STAGE))
	$(call stage_esp,$(ESP_STAGE))
	@LIMINE=$(LIMINE_DIR)/limine ./toolchain/mkdisk.sh $@ $(DISK_MIB) $(ESP_MIB) $(DISK_STAGE) $(ESP_STAGE)

# boot partition-এর ফাইল: factory kernel (gcc), initrd, Limine (BIOS + UEFI), প্রথম মেনু
define stage_esp
	@rm -rf $(1) && mkdir -p $(1)/EFI/BOOT $(1)/kernels
	@cp $(KERNEL) $(1)/rozos-factory.elf
	@cp $(INITRD) $(1)/initrd.tar
	@cp limine-disk.conf $(1)/limine.conf
	@cp $(LIMINE_DIR)/limine-bios.sys $(1)/
	@cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(1)/EFI/BOOT/
	@echo "factory" > $(1)/kernels/default
	@: > $(1)/kernels/list
endef

# ধাপ ১১খ: পুরোনো ডিস্ক (partition ছাড়া পুরোটা ext2) → নতুন GPT ডিস্ক; সব ফাইল অক্ষত
# (পুরোনোটা হুবহু sda2-তে, মাপ বাড়ানো)। পুরোনো image-এর কপি $(DISK).old-ext2-তে থাকে।
disk-migrate: $(KERNEL) $(INITRD) $(LIMINE_FILES)
	@test -f $(DISK) || { echo "$(DISK) নেই — make disk দিয়ে নতুন বানান"; exit 1; }
	@if [ "$$(./toolchain/disk-offsets.sh $(DISK))" != old ]; then echo "  OK      $(DISK) আগেই partition-সহ (নতুন ধরনের)"; exit 0; fi; \
	 echo "  MIGRATE $(DISK): পুরোনো ext2 → GPT (FAT32 boot + ext2 root); কপি: $(DISK).old-ext2"; \
	 cp $(DISK) $(DISK).old-ext2
	$(call stage_esp,$(ESP_STAGE))
	@if [ "$$(./toolchain/disk-offsets.sh $(DISK))" = old ]; then \
	   LIMINE=$(LIMINE_DIR)/limine ./toolchain/mkdisk.sh $(DISK) $(DISK_MIB) $(ESP_MIB) $(DISK_STAGE) $(ESP_STAGE) $(DISK).old-ext2 && \
	   rm -f $(DISK_BIN_STAMP); fi

# ধাপ ১২ক: ডিস্ক বড় করা (যেমন make disk-grow DISK_MIB=1024) — তারপর disk-update
disk-grow: | $(DISK)
	@./toolchain/disk-grow.sh $(DISK) $(DISK_MIB)
	@rm -f $(DISK_BIN_STAMP)
	@$(MAKE) --no-print-directory disk-update

# ধাপ ১৫ক: NVMe পরীক্ষার ডিস্ক — পুরোটা FAT32 (পেন ড্রাইভের মতো), ভেতরে README.TXT
$(NVME_IMG):
	@echo "  NVME    $@ (64 MiB FAT32, label ROZNVME)"
	@rm -f $@ && truncate -s 64M $@ && mkfs.fat -F 32 -n ROZNVME $@ >/dev/null
	@printf 'Hello from the NVMe disk!\nmount /dev/nvmea /mnt/nvme  then  ls /mnt/nvme\n' > $(BUILD)/nvme-readme.txt 2>/dev/null || { mkdir -p $(BUILD); printf 'Hello from the NVMe disk!\n' > $(BUILD)/nvme-readme.txt; }
	@mcopy -i $@ $(BUILD)/nvme-readme.txt ::README.TXT

# ধাপ ১৫খ: পরীক্ষার পেন ড্রাইভ — MBR, একটা FAT32 partition (১ MiB থেকে), ভেতরে README.TXT
$(USB_IMG):
	@echo "  USB     $@ (64 MiB, MBR + FAT32 partition, label ROZUSB)"
	@rm -f $@ && truncate -s 64M $@
	@mkdir -p $(BUILD)
	@# MBR হাতে: partition 1 = টাইপ 0x0C (FAT32 LBA), LBA 2048 থেকে 129024 sector; শেষে 55 AA
	@printf '\000\040\041\000\014\376\377\377\000\010\000\000\000\370\001\000' | dd of=$@ bs=1 seek=446 conv=notrunc status=none
	@printf '\125\252' | dd of=$@ bs=1 seek=510 conv=notrunc status=none
	@mkfs.fat -F 32 -n ROZUSB --offset=2048 $@ >/dev/null
	@printf 'Hello from the USB pen drive!\nrozOS mounts it on /mnt/usb - umount /mnt/usb before pulling it out.\n' > $(BUILD)/usb-readme.txt
	@mcopy -i $@@@1M $(BUILD)/usb-readme.txt ::README.TXT

disk-reset:
	rm -f $(DISK)
	@$(MAKE) --no-print-directory disk

disk-check: $(DISK)
	@o=$$(./toolchain/disk-offsets.sh $(DISK)); if [ "$$o" = old ]; then e2fsck -fn $(DISK); else \
	 set -- $$o; e2fsck -fn "$(DISK)?offset=$$2"; \
	 command -v fsck.fat >/dev/null && { dd if=$(DISK) of=$(BUILD)/esp-check.img bs=512 skip=$$(($$1 / 512)) count=$$(($(ESP_MIB) * 2048)) status=none; \
	 fsck.fat -n $(BUILD)/esp-check.img; rm -f $(BUILD)/esp-check.img; }; fi

# ধাপ ৬: ডিস্কের /bin-এ শুধু আমাদের প্রোগ্রামগুলো বদলানো (debugfs দিয়ে) — rozOS-এর
# (ধাপ ৯: /etc/hosts শুধু না থাকলে লেখা হয় — debugfs আগের ফাইল বদলায় না; ধাপ ১০-এর
#  /etc/rozpkg.conf-ও তাই। /etc/ssl/certs.pem, /usr/lib-এর lib আর হেডার প্রতিবার নতুন)
# ভেতরে লেখা বাকি সব ফাইল থাকে। QEMU চলার সময় চালাবেন না।
disk-update: $(DISK_BIN_STAMP)

$(DISK_BIN_STAMP): $(USER_STRIPPED) $(LIBC_A) $(LIBC_CRT0) $(LIBC_CRTS) $(TCC_EXTRA) $(HELP_FILES) $(APP_FILES) $(TEMPLATE_FILES) $(FONT_FILES) $(IMAGE_FILES) $(ICON_FILES) $(GFX_SRC_FILES) $(MOD_KOS) $(CA_PEM) $(DEV_LIBS) $(DEV_HEADERS) $(KSRC_MK) $(KSRC_FILES) $(KERNEL) $(INITRD) | $(DISK)
	@command -v debugfs >/dev/null || { echo "debugfs নেই — sudo apt install e2fsprogs"; exit 1; }
	@if [ "$$(./toolchain/disk-offsets.sh $(DISK))" = old ]; then $(MAKE) --no-print-directory disk-migrate; fi
	@./toolchain/disk-grow.sh $(DISK) $(DISK_MIB)
	@echo "  UPDATE  $(DISK): /bin ($(words $(USER_PROGS)) programs), /usr/lib, /usr/include, /usr/share/help, /usr/share/apps, /etc/ssl, /usr/src/rozos$(if $(HAVE_TCC),$(comma) /usr/src/tcc)"
	@{ echo "mkdir /bin"; echo "mkdir /usr"; echo "mkdir /usr/lib"; echo "mkdir /usr/include"; \
	   for p in $(USER_PROGS); do echo "rm /bin/$$p"; echo "write $(USER_BUILD)/stripped/$$p /bin/$$p"; \
	   echo "sif /bin/$$p mode 0100755"; echo "sif /bin/$$p uid 0"; echo "sif /bin/$$p gid 0"; done; \
	   for f in $(notdir $(LIBC_A) $(LIBC_CRT0) $(LIBC_CRTS)); do echo "rm /usr/lib/$$f"; \
	   echo "write $(LIBC_BUILD)/$$f /usr/lib/$$f"; done; \
	   echo "rm /usr/lib/crt1.o"; echo "write $(LIBC_CRT0) /usr/lib/crt1.o"; \
	   echo "write sysroot/etc/hosts /etc/hosts"; \
	   echo "write sysroot/etc/rozos.conf /etc/rozos.conf"; \
	   echo "write sysroot/etc/rozpkg.conf /etc/rozpkg.conf"; \
	   echo "mkdir /etc/ssl"; echo "rm /etc/ssl/certs.pem"; echo "write $(CA_PEM) /etc/ssl/certs.pem"; \
	   echo "mkdir /var"; echo "mkdir /var/cache"; echo "mkdir /var/cache/rozpkg"; \
	   echo "mkdir /var/lib"; echo "mkdir /var/lib/rozpkg"; echo "mkdir /var/lib/rozpkg/installed"; \
	   for f in $(DEV_LIBS); do echo "rm /usr/lib/$$(basename $$f)"; echo "write $$f /usr/lib/$$(basename $$f)"; done; \
	   for f in $(DEV_HEADERS); do echo "rm /usr/include/$$(basename $$f)"; echo "write $$f /usr/include/$$(basename $$f)"; done; \
	   echo "mkdir /usr/local"; echo "mkdir /usr/local/bin"; echo "mkdir /home/bin"; \
	   echo "mkdir /usr/share"; echo "mkdir /usr/share/apps"; \
	   for f in $(APP_FILES); do echo "rm /usr/share/apps/$$(basename $$f)"; echo "write $$f /usr/share/apps/$$(basename $$f)"; done; \
	   echo "mkdir /usr/share/templates"; \
	   for f in $(TEMPLATE_FILES); do echo "rm /usr/share/templates/$$(basename $$f)"; echo "write $$f /usr/share/templates/$$(basename $$f)"; done; \
	   echo "mkdir /usr/share/fonts"; echo "mkdir /usr/share/images"; \
	   for f in $(FONT_FILES); do echo "rm /usr/share/fonts/$$(basename $$f)"; echo "write $$f /usr/share/fonts/$$(basename $$f)"; done; \
	   for f in $(IMAGE_FILES); do echo "rm /usr/share/images/$$(basename $$f)"; echo "write $$f /usr/share/images/$$(basename $$f)"; done; \
	   echo "mkdir /usr/share/icons"; \
	   echo "mkdir /lib"; echo "mkdir /lib/modules"; echo "mkdir /usr/src/modules"; \
	   echo "mkdir /mnt"; \
	   for f in $(MOD_KOS); do echo "rm /lib/modules/$$(basename $$f)"; echo "write $$f /lib/modules/$$(basename $$f)"; done; \
	   for f in $(MOD_SRCS); do echo "rm /usr/src/modules/$$(basename $$f)"; echo "write $$f /usr/src/modules/$$(basename $$f)"; done; \
	   for f in $(ICON_FILES); do echo "rm /usr/share/icons/$$(basename $$f)"; echo "write $$f /usr/share/icons/$$(basename $$f)"; done; \
	   echo "mkdir /usr/src"; echo "mkdir /usr/src/gfx"; \
	   for f in $(GFX_SRC_FILES); do echo "rm /usr/src/gfx/$$(basename $$f)"; echo "write $$f /usr/src/gfx/$$(basename $$f)"; done; \
	   echo "mkdir /usr/src"; echo "mkdir /usr/src/rozos"; \
	   for d in $(KSRC_DIRS); do echo "mkdir /usr/src/rozos/$$d"; done; \
	   for f in $(KSRC_FILES); do echo "rm /usr/src/rozos/$$f"; echo "write $$f /usr/src/rozos/$$f"; done; \
	   echo "rm /usr/src/rozos/Makefile"; echo "write $(KSRC_MK) /usr/src/rozos/Makefile"; \
	   echo "rm /usr/src/rozos/objs.list"; echo "write $(KSRC_OBJS) /usr/src/rozos/objs.list"; \
	   echo "mkdir /usr/share"; echo "mkdir /usr/share/help"; \
	   for f in $(HELP_FILES); do echo "rm /usr/share/help/$$(basename $$f)"; \
	   echo "write $$f /usr/share/help/$$(basename $$f)"; done; \
	   $(if $(HAVE_TCC),echo "mkdir /usr/lib/tcc"; echo "mkdir /usr/lib/tcc/include"; \
	   echo "rm /usr/lib/tcc/libtcc1.a"; echo "write $(TCC_LIB1) /usr/lib/tcc/libtcc1.a"; \
	   echo "rm /usr/lib/tcc/runmain.o"; echo "write $(TCC_RUNMAIN) /usr/lib/tcc/runmain.o"; \
	   for f in $(notdir $(wildcard $(TCC_SRC)/include/*.h)); do echo "rm /usr/lib/tcc/include/$$f"; \
	   echo "write $(TCC_SRC)/include/$$f /usr/lib/tcc/include/$$f"; done; \
	   echo "mkdir /usr/src"; echo "mkdir /usr/src/tcc"; echo "mkdir /usr/src/tcc/include"; echo "mkdir /usr/src/tcc/lib"; \
	   for f in $(TCC_SRC_FILES) ports/tcc/config.h ports/tcc/build-in-rozos.sh; do echo "rm /usr/src/tcc/$$(basename $$f)"; \
	   echo "write $$f /usr/src/tcc/$$(basename $$f)"; done; \
	   for f in $(wildcard $(TCC_SRC)/include/* $(TCC_SRC)/lib/*); do echo "rm /usr/src/tcc/$${f#$(TCC_SRC)/}"; \
	   echo "write $$f /usr/src/tcc/$${f#$(TCC_SRC)/}"; done;) \
	   cd libc/include && for d in $$(find . -mindepth 1 -type d | sed 's|^\./||'); do echo "mkdir /usr/include/$$d"; done; \
	   for f in $$(find . -type f | sed 's|^\./||'); do echo "rm /usr/include/$$f"; \
	   echo "write libc/include/$$f /usr/include/$$f"; done; } > $(BUILD)/disk-update.cmd
	@set -- $$(./toolchain/disk-offsets.sh $(DISK)); \
	 { cat $(BUILD)/disk-update.cmd; echo "rm /usr/src/rozos/.build-base"; \
	   echo "write $(BUILD_NUMBER_FILE) /usr/src/rozos/.build-base"; } > $(BUILD)/disk-update.cmd2; \
	 debugfs -w -f $(BUILD)/disk-update.cmd2 "$(DISK)?offset=$$2" >/dev/null 2>&1; \
	 echo "  UPDATE  boot partition: factory kernel (build $$(cat $(BUILD_NUMBER_FILE) 2>/dev/null)), initrd, Limine"; \
	 M="$(DISK)@@$$1"; \
	 mcopy -o -i "$$M" $(KERNEL) ::/rozos-factory.elf && \
	 mcopy -o -i "$$M" $(INITRD) ::/initrd.tar && \
	 mcopy -o -i "$$M" $(LIMINE_DIR)/limine-bios.sys ::/limine-bios.sys && \
	 mcopy -o -i "$$M" $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI ::/EFI/BOOT/ && \
	 { mdir -i "$$M" ::/limine.conf >/dev/null 2>&1 || mcopy -i "$$M" limine-disk.conf ::/limine.conf; } && \
	 { mdir -i "$$M" ::/kernels >/dev/null 2>&1 || mmd -i "$$M" ::/kernels; } && \
	 ./toolchain/disk-menu.sh $(DISK) $$1 </dev/null && \
	 $(LIMINE_DIR)/limine bios-install $(DISK) 3 >/dev/null 2>&1
	@touch $@

# ---------- ISO ----------
LIMINE_FILES := $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
                $(LIMINE_DIR)/limine-uefi-cd.bin $(LIMINE_DIR)/BOOTX64.EFI \
                $(LIMINE_DIR)/BOOTIA32.EFI

$(LIMINE_DIR)/limine:
	@echo "Limine পাওয়া যায়নি — আগে ./toolchain/get-limine.sh চালান" && exit 1

iso: $(ISO)

# $(1) = kernel ELF, $(2) = ISO ফাইল, $(3) = অস্থায়ী ডিরেক্টরি (ধাপ ১১: self-iso-ও এটা ব্যবহার করে)
define make_iso
	@echo "  ISO     $(2)"
	@rm -rf $(3)
	@mkdir -p $(3)/boot/limine $(3)/EFI/BOOT
	@cp $(1) $(3)/boot/rozos.elf
	@cp $(INITRD) $(3)/boot/initrd.tar
	@cp limine.conf $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	    $(LIMINE_DIR)/limine-uefi-cd.bin $(3)/boot/limine/
	@cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(3)/EFI/BOOT/
	@xorriso -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(3) -o $(2) 2>/dev/null
	@./$(LIMINE_DIR)/limine bios-install $(2) >/dev/null 2>&1
	@echo "  DONE    $(2)"
endef

$(ISO): $(KERNEL) $(INITRD) limine.conf $(LIMINE_DIR)/limine $(LIMINE_FILES)
	$(call make_iso,$(KERNEL),$@,$(ISO_ROOT))

# ধাপ ১১: rozOS-এর ভেতরে `cd /usr/src/rozos && make` দিয়ে বানানো kernel ডিস্ক থেকে বের করে
# আলাদা ISO (initrd একই)। QEMU চলার সময় চালাবেন না। তারপর: make run-self
self-iso: $(INITRD) limine.conf $(LIMINE_DIR)/limine $(LIMINE_FILES) | $(DISK)
	@command -v debugfs >/dev/null || { echo "debugfs নেই — sudo apt install e2fsprogs"; exit 1; }
	@mkdir -p $(dir $(SELF_KERNEL))
	@rm -f $(SELF_KERNEL)
	@o=$$(./toolchain/disk-offsets.sh $(DISK)); if [ "$$o" = old ]; then img="$(DISK)"; else set -- $$o; img="$(DISK)?offset=$$2"; fi; \
	 debugfs -R "dump /usr/src/rozos/build/rozos.elf $(SELF_KERNEL)" "$$img" >/dev/null 2>&1 || true
	@test -s $(SELF_KERNEL) || { echo "ডিস্কে /usr/src/rozos/build/rozos.elf নেই — আগে rozOS-এ: cd /usr/src/rozos && make"; exit 1; }
	@echo "  KERNEL  $(SELF_KERNEL) ($$(stat -c %s $(SELF_KERNEL)) bytes, built inside rozOS)"
	$(call make_iso,$(SELF_KERNEL),$(SELF_ISO),$(BUILD)/iso_root_self)

run-self: self-iso $(DISK_BIN_STAMP)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) $(QEMU_DISK) -cdrom $(SELF_ISO) -boot d -display none $(QEMU_SERIAL)

comma := ,

# ধাপ ৯: নেটওয়ার্ক — QEMU-র user-mode (slirp): rozOS = 10.0.2.15, হোস্ট/গেটওয়ে = 10.0.2.2,
# DNS = 10.0.2.3। কার্ড e1000e (82574L), q35-এর ডিফল্টই, এখানে স্পষ্ট করে লেখা।
QEMU_NET ?= -nic user,model=e1000e

# ধাপ ৭: serial টার্মিনাল — Ctrl+C আর QEMU বন্ধ করে না, rozOS-এ যায় (shell/এডিটরে লাগে)।
# QEMU থেকে বের হওয়া: Ctrl+A তারপর X  (Ctrl+A দুবার = rozOS-এ একটা Ctrl+A)
QEMU_SERIAL = -chardev stdio,id=con0,mux=on,signal=off -serial chardev:con0

# ---------- QEMU ----------
# ধাপ ১১খ: ডিস্ক থেকেই boot (Limine ডিস্কে, মেনুতে kinstall-এর kernel-গুলো)।
# ISO থেকে আগের মতো: make run-iso / run-iso-serial (rescue)।
run: $(DISK_BIN_STAMP) | $(DISK) $(NVME_DEP) $(USB_DEP)
	@echo "  QEMU    $(QEMU_MEM) RAM, $(if $(filter 1,$(KVM)),KVM (fast),TCG emulation - no KVM: help qemu)"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) $(QEMU_DISK) -boot c $(QEMU_SERIAL)

run-serial: $(DISK_BIN_STAMP) | $(DISK) $(NVME_DEP) $(USB_DEP)
	@echo "  QEMU    $(QEMU_MEM) RAM, $(if $(filter 1,$(KVM)),KVM (fast),TCG emulation - no KVM: help qemu)"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) $(QEMU_DISK) -boot c -display none $(QEMU_SERIAL)

run-iso: $(ISO) $(DISK_BIN_STAMP) | $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) $(QEMU_DISK) -cdrom $(ISO) -boot d $(QEMU_SERIAL)

run-iso-serial: $(ISO) $(DISK_BIN_STAMP) | $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) $(QEMU_DISK) -cdrom $(ISO) -boot d -display none $(QEMU_SERIAL)

$(BUILD)/ovmf_vars.fd:
	@mkdir -p $(BUILD)
	@cp $(OVMF_VARS) $@

run-uefi: $(BUILD)/ovmf_vars.fd $(DISK_BIN_STAMP) | $(DISK) $(NVME_DEP) $(USB_DEP)
	@echo "  QEMU    $(QEMU_MEM) RAM, $(if $(filter 1,$(KVM)),KVM (fast),TCG emulation - no KVM: help qemu)"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) $(QEMU_DISK) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -boot c $(QEMU_SERIAL)

run-uefi-iso: $(ISO) $(BUILD)/ovmf_vars.fd $(DISK_BIN_STAMP) | $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_NET) $(QEMU_DISK) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -cdrom $(ISO) -boot d $(QEMU_SERIAL)

debug: $(ISO) $(DISK_BIN_STAMP) | $(DISK)
	@echo "QEMU থেমে আছে — অন্য টার্মিনালে 'make gdb' চালান"
	$(QEMU) $(QEMU_BASE_FLAGS) $(QEMU_DEBUG_FLAGS) $(QEMU_NET) $(QEMU_DISK) -cdrom $(ISO) -boot d $(QEMU_SERIAL) -s -S -d int,cpu_reset -D $(BUILD)/qemu.log

gdb: $(KERNEL)
	gdb $(KERNEL) -ex "target remote localhost:1234" -ex "break kmain" -ex "continue"

# ---------- সহায়ক ----------
check-tools:
	@for t in $(CC) xorriso $(QEMU) mke2fs e2fsck debugfs sgdisk mkfs.fat mcopy; do \
	    command -v $$t >/dev/null && echo "  ok      $$t" || echo "  MISSING $$t"; \
	done
	@test -x $(LIMINE_DIR)/limine && echo "  ok      $(LIMINE_DIR)/limine" || echo "  MISSING $(LIMINE_DIR)/limine"
	@test -f kernel/boot/limine.h && echo "  ok      kernel/boot/limine.h" || echo "  MISSING kernel/boot/limine.h"
ifeq ($(ENABLE_KCXX),1)
	@command -v $(CXX) >/dev/null && echo "  ok      $(CXX)" || echo "  MISSING $(CXX)"
endif

info:
	@echo "TOOLCHAIN   = $(TOOLCHAIN)"
	@echo "CC          = $(CC)"
	@echo "CXX         = $(CXX)"
	@echo "ENABLE_KCXX = $(ENABLE_KCXX)"
	@echo "C sources   = $(words $(C_SRCS)), asm = $(words $(S_SRCS)), C++ = $(words $(CXX_SRCS))"
	@echo "user progs  = $(USER_PROGS)"

clean:
	rm -rf $(BUILD) $(ISO) $(SELF_ISO)
