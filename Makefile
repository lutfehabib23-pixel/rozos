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
DISK_SIZE  ?= 64M
DISK_STAGE := $(BUILD)/disk-root
# q35-এ index=0 মানে AHCI-র প্রথম পোর্ট (CD-ROM থাকে index 2-এ)
QEMU_DISK  = -drive file=$(DISK),format=raw,index=0,media=disk

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
USER_DIRS    := init shell editor coreutils bin tests
USER_SRCS    := $(foreach d,$(USER_DIRS),$(wildcard userland/$(d)/*.c))
USER_PROGS   := $(sort $(notdir $(basename $(USER_SRCS))))
USER_BINS    := $(addprefix $(USER_BUILD)/bin/,$(USER_PROGS))
# rozOS-এ যায় debug-তথ্য ছাড়া কপি (৪-৫ গুণ ছোট); build/userland/bin-এরগুলো gdb-র জন্য থাকে
USER_STRIPPED := $(addprefix $(USER_BUILD)/stripped/,$(USER_PROGS))
USER_CFLAGS  := -std=c99 $(OPT) -g -pipe -Wall -Wextra -m64 -march=x86-64 \
                -ffreestanding -nostdinc -isystem $(CC_INCLUDE) -Ilibc/include \
                -fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables
LIBC_CFLAGS  := $(USER_CFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns
USER_LDFLAGS := -nostdlib -static -no-pie -Wl,-z,max-page-size=0x1000 \
                -Wl,--build-id=none -Wl,-Ttext-segment=0x400000
vpath %.c $(addprefix userland/,$(USER_DIRS))

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

.PHONY: all kernel libc userland tcc-check disk-update initrd iso disk disk-reset disk-check run run-uefi run-serial debug gdb clean check-tools info

all: kernel userland

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
$(KERNEL): $(OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CC) $(KERNEL_LDFLAGS) $(OBJS) $(KERNEL_LIBS) -o $@

-include $(DEPS)
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
	@$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(USER_BUILD)/bin/%: $(USER_BUILD)/obj/%.o $(LIBC_A) $(LIBC_CRT0)
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(CC) $(USER_LDFLAGS) $(LIBC_CRT0) $< $(LIBC_A) -lgcc -o $@

$(USER_BUILD)/stripped/%: $(USER_BUILD)/bin/%
	@mkdir -p $(dir $@)
	@$(USER_STRIP) -o $@ $<

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

# মধ্যবর্তী .o ফাইল make যেন মুছে না ফেলে (নইলে প্রতিবার আবার বিল্ড)
.SECONDARY: $(LIBC_OBJS) $(LIBC_CRT0) $(LIBC_CRTS) $(TCC_LIB1_OBJS) $(TCC_RUNMAIN) $(TCC_BUILD)/tcc.o $(patsubst %,$(USER_BUILD)/obj/%.o,$(USER_PROGS)) $(USER_BINS)

# sysroot + /bin-এর প্রোগ্রাম + rozlibc (/usr/include, /usr/lib — ধাপ ৮-এর TCC-র জন্য)
define stage_root
	@rm -rf $(1) && mkdir -p $(1)
	@tar -C $(SYSROOT) --exclude=.keep -cf - . | tar -C $(1) -xf -
	@mkdir -p $(1)/bin $(1)/tmp $(1)/dev $(1)/usr/include $(1)/usr/lib
	@cp $(USER_STRIPPED) $(1)/bin/
	@cp -r libc/include/. $(1)/usr/include/
	@cp $(LIBC_A) $(LIBC_CRT0) $(LIBC_CRTS) $(1)/usr/lib/
	@cp $(LIBC_CRT0) $(1)/usr/lib/crt1.o
	@mkdir -p $(1)/usr/share/help && cp $(HELP_FILES) $(1)/usr/share/help/
	$(if $(HAVE_TCC),@mkdir -p $(1)/usr/lib/tcc/include && cp $(TCC_SRC)/include/*.h $(1)/usr/lib/tcc/include/ && cp $(TCC_LIB1) $(TCC_RUNMAIN) $(1)/usr/lib/tcc/)
	$(if $(HAVE_TCC),@mkdir -p $(1)/usr/src/tcc && cp $(TCC_SRC_FILES) ports/tcc/config.h ports/tcc/build-in-rozos.sh $(1)/usr/src/tcc/ && cp -r $(TCC_SRC)/include $(TCC_SRC)/lib $(1)/usr/src/tcc/)
endef

# ---------- initrd ----------
# USTAR ফরম্যাট (কার্নেলের initrd.c এটাই পড়ে); মালিক root, যাতে হোস্টের ইউজার নাম না ঢোকে
initrd: $(INITRD)

$(INITRD): $(SYSROOT_FILES) $(USER_STRIPPED) $(LIBC_A) $(LIBC_CRTS) $(TCC_EXTRA) $(HELP_FILES)
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
$(DISK): | $(USER_STRIPPED) $(LIBC_A) $(LIBC_CRTS) $(TCC_EXTRA)
	@command -v mke2fs >/dev/null || { echo "mke2fs নেই — sudo apt install e2fsprogs"; exit 1; }
	@echo "  MKFS    $@ ($(DISK_SIZE) ext2 from $(SYSROOT)/ + build/userland/bin)"
	$(call stage_root,$(DISK_STAGE))
	@mke2fs -q -F -t ext2 -b 4096 -I 256 -L rozos -E root_owner=0:0 -d $(DISK_STAGE) $@ $(DISK_SIZE)

disk-reset:
	rm -f $(DISK)
	@$(MAKE) --no-print-directory disk

disk-check: $(DISK)
	e2fsck -fn $(DISK)

# ধাপ ৬: ডিস্কের /bin-এ শুধু আমাদের প্রোগ্রামগুলো বদলানো (debugfs দিয়ে) — rozOS-এর
# ভেতরে লেখা বাকি সব ফাইল থাকে। QEMU চলার সময় চালাবেন না।
disk-update: $(DISK_BIN_STAMP)

$(DISK_BIN_STAMP): $(USER_STRIPPED) $(LIBC_A) $(LIBC_CRT0) $(LIBC_CRTS) $(TCC_EXTRA) $(HELP_FILES) | $(DISK)
	@command -v debugfs >/dev/null || { echo "debugfs নেই — sudo apt install e2fsprogs"; exit 1; }
	@echo "  UPDATE  $(DISK): /bin ($(words $(USER_PROGS)) programs), /usr/lib, /usr/include, /usr/share/help$(if $(HAVE_TCC),$(comma) /usr/src/tcc)"
	@{ echo "mkdir /bin"; echo "mkdir /usr"; echo "mkdir /usr/lib"; echo "mkdir /usr/include"; \
	   for p in $(USER_PROGS); do echo "rm /bin/$$p"; echo "write $(USER_BUILD)/stripped/$$p /bin/$$p"; \
	   echo "sif /bin/$$p mode 0100755"; echo "sif /bin/$$p uid 0"; echo "sif /bin/$$p gid 0"; done; \
	   for f in $(notdir $(LIBC_A) $(LIBC_CRT0) $(LIBC_CRTS)); do echo "rm /usr/lib/$$f"; \
	   echo "write $(LIBC_BUILD)/$$f /usr/lib/$$f"; done; \
	   echo "rm /usr/lib/crt1.o"; echo "write $(LIBC_CRT0) /usr/lib/crt1.o"; \
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
	@debugfs -w -f $(BUILD)/disk-update.cmd $(DISK) >/dev/null 2>&1
	@touch $@

# ---------- ISO ----------
LIMINE_FILES := $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
                $(LIMINE_DIR)/limine-uefi-cd.bin $(LIMINE_DIR)/BOOTX64.EFI \
                $(LIMINE_DIR)/BOOTIA32.EFI

$(LIMINE_DIR)/limine:
	@echo "Limine পাওয়া যায়নি — আগে ./toolchain/get-limine.sh চালান" && exit 1

iso: $(ISO)

$(ISO): $(KERNEL) $(INITRD) limine.conf $(LIMINE_DIR)/limine $(LIMINE_FILES)
	@echo "  ISO     $@"
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot/limine $(ISO_ROOT)/EFI/BOOT
	@cp $(KERNEL) $(ISO_ROOT)/boot/rozos.elf
	@cp $(INITRD) $(ISO_ROOT)/boot/initrd.tar
	@cp limine.conf $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	    $(LIMINE_DIR)/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/
	@cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/
	@xorriso -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(ISO_ROOT) -o $@ 2>/dev/null
	@./$(LIMINE_DIR)/limine bios-install $@ >/dev/null 2>&1
	@echo "  DONE    $@"

comma := ,

# ধাপ ৭: serial টার্মিনাল — Ctrl+C আর QEMU বন্ধ করে না, rozOS-এ যায় (shell/এডিটরে লাগে)।
# QEMU থেকে বের হওয়া: Ctrl+A তারপর X  (Ctrl+A দুবার = rozOS-এ একটা Ctrl+A)
QEMU_SERIAL = -chardev stdio,id=con0,mux=on,signal=off -serial chardev:con0

# ---------- QEMU ----------
run: $(ISO) $(DISK_BIN_STAMP) | $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK) -cdrom $(ISO) -boot d $(QEMU_SERIAL)

run-serial: $(ISO) $(DISK_BIN_STAMP) | $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK) -cdrom $(ISO) -boot d -display none $(QEMU_SERIAL)

$(BUILD)/ovmf_vars.fd:
	@mkdir -p $(BUILD)
	@cp $(OVMF_VARS) $@

run-uefi: $(ISO) $(BUILD)/ovmf_vars.fd $(DISK_BIN_STAMP) | $(DISK)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -cdrom $(ISO) -boot d $(QEMU_SERIAL)

debug: $(ISO) $(DISK_BIN_STAMP) | $(DISK)
	@echo "QEMU থেমে আছে — অন্য টার্মিনালে 'make gdb' চালান"
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DISK) -cdrom $(ISO) -boot d $(QEMU_SERIAL) -s -S -d int,cpu_reset -D $(BUILD)/qemu.log

gdb: $(KERNEL)
	gdb $(KERNEL) -ex "target remote localhost:1234" -ex "break kmain" -ex "continue"

# ---------- সহায়ক ----------
check-tools:
	@for t in $(CC) xorriso $(QEMU) mke2fs e2fsck debugfs; do \
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
	rm -rf $(BUILD) $(ISO)
