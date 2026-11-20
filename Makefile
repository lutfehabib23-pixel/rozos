# rozOS — টপ-লেভেল Makefile
#
#   make            কার্নেল বিল্ড (build/rozos.elf)
#   make initrd     sysroot/ থেকে build/initrd.tar (USTAR)
#   make iso        বুটযোগ্য ISO (rozos.iso) — BIOS ও UEFI দুটোতেই চলে
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

.PHONY: all kernel initrd iso run run-uefi run-serial debug gdb clean check-tools info

all: kernel

kernel: $(KERNEL)

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

# ---------- initrd ----------
# USTAR ফরম্যাট (কার্নেলের initrd.c এটাই পড়ে); মালিক root, যাতে হোস্টের ইউজার নাম না ঢোকে
initrd: $(INITRD)

$(INITRD): $(SYSROOT_FILES)
	@mkdir -p $(dir $@)
	@echo "  TAR     $@"
	@tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name \
	    -cf $@ -C $(SYSROOT) .

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

# ---------- QEMU ----------
run: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO) -boot d -serial stdio

run-serial: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO) -boot d -display none -serial stdio

$(BUILD)/ovmf_vars.fd:
	@mkdir -p $(BUILD)
	@cp $(OVMF_VARS) $@

run-uefi: $(ISO) $(BUILD)/ovmf_vars.fd
	$(QEMU) $(QEMU_FLAGS) \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
	    -cdrom $(ISO) -boot d -serial stdio

debug: $(ISO)
	@echo "QEMU থেমে আছে — অন্য টার্মিনালে 'make gdb' চালান"
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO) -boot d -serial stdio -s -S -d int,cpu_reset -D $(BUILD)/qemu.log

gdb: $(KERNEL)
	gdb $(KERNEL) -ex "target remote localhost:1234" -ex "break kmain" -ex "continue"

# ---------- সহায়ক ----------
check-tools:
	@for t in $(CC) xorriso $(QEMU); do \
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

clean:
	rm -rf $(BUILD) $(ISO)
