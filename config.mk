# rozOS — বিল্ড কনফিগারেশন
# টুলচেইন বা ফ্ল্যাগ বদলাতে শুধু এই ফাইল (বা make কমান্ডে ভেরিয়েবল) বদলান।
#   উদাহরণ:  make ENABLE_KCXX=0      — কার্নেলের C++ অংশ বাদ দিয়ে বিল্ড
#             make OPT=-O0            — ডিবাগিংয়ের জন্য অপ্টিমাইজেশন বন্ধ

# ---------- টুলচেইন ----------
# gcc : হোস্টের x86_64-elf ক্রস-কম্পাইলার (toolchain/build-cross.sh)
# tcc : rozOS-এর ভেতরের সেলফ-হোস্টিং বিল্ড (ধাপ ৮-১১-এ চালু হবে)
TOOLCHAIN ?= gcc
CROSS     ?= x86_64-elf-

ifeq ($(TOOLCHAIN),gcc)
  CC          := $(CROSS)gcc
  CXX         := $(CROSS)g++
  ENABLE_KCXX ?= 1
else ifeq ($(TOOLCHAIN),tcc)
  CC          := tcc
  CXX         :=
  # TCC C++ কম্পাইল করতে পারে না — kernel/cxx/ সবসময় বাদ
  override ENABLE_KCXX := 0
  $(error TOOLCHAIN=tcc এখনো চালু হয়নি — ধাপ ৮-এ TCC পোর্টের পর linker script সাপোর্ট যাচাই করে চালু হবে)
else
  $(error অজানা TOOLCHAIN '$(TOOLCHAIN)' — gcc অথবা tcc দিন)
endif

# ---------- কার্নেলের সাধারণ ফ্ল্যাগ ----------
OPT ?= -O2

# x86_64 কার্নেলের জন্য বাধ্যতামূলক:
#  -mno-red-zone    : ইন্টারাপ্ট স্ট্যাকের red zone নষ্ট করে, তাই বন্ধ
#  -mno-sse/mmx/... : কার্নেল FPU/SIMD রেজিস্টার সেভ করে না (PROTOCOL.md: ABI without FP/SIMD)
#  -mcmodel=kernel  : কার্নেল ঠিকানা 0xffffffff80000000-এর উপরে (শেষ 2 GiB)
KERNEL_ARCH_FLAGS := -m64 -march=x86-64 -mno-red-zone -mcmodel=kernel \
                     -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mgeneral-regs-only

KERNEL_COMMON_FLAGS := $(OPT) -g -pipe -Wall -Wextra \
                       -ffreestanding -fno-stack-protector -fno-stack-check \
                       -fno-pie -fno-pic -fno-lto -fno-omit-frame-pointer \
                       -fno-asynchronous-unwind-tables -fno-unwind-tables \
                       -ffunction-sections -fdata-sections \
                       $(KERNEL_ARCH_FLAGS) \
                       -nostdinc -isystem $(shell $(CC) -print-file-name=include) \
                       -Ikernel/include -Ikernel -MMD -MP

# C: C99 (TCC-বান্ধব) — GCC-only extension ব্যবহার করবেন না; inline asm-এ __asm__ লিখুন
# GCC 14+ এগুলোকে সবসময় error ধরে — পুরোনো gcc-তেও একই রকম কড়া রাখা (শুধু C)
KERNEL_CFLAGS := -std=c99 $(KERNEL_COMMON_FLAGS) -Werror=implicit-function-declaration \
                 -Werror=int-conversion -Werror=incompatible-pointer-types

# Assembly (.S): GAS/AT&T সিনট্যাক্স, C preprocessor সহ
KERNEL_ASFLAGS := $(KERNEL_COMMON_FLAGS) -D__ASSEMBLY__

# C++ (শুধু kernel/cxx/, ENABLE_KCXX=1 হলে)
KERNEL_CXXFLAGS := -std=c++17 $(KERNEL_COMMON_FLAGS) \
                   -fno-exceptions -fno-rtti -fno-threadsafe-statics

ifeq ($(ENABLE_KCXX),1)
  KERNEL_CFLAGS   += -DROZOS_KCXX=1
  KERNEL_CXXFLAGS += -DROZOS_KCXX=1
endif

# লিংক: -mno-red-zone দিলে GCC নিজে থেকেই no-red-zone/libgcc.a বেছে নেয়
KERNEL_LDFLAGS := -nostdlib -static -no-pie $(KERNEL_ARCH_FLAGS) \
                  -Wl,-T,kernel/linker.ld -Wl,--build-id=none \
                  -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections
KERNEL_LIBS    := -lgcc

# ---------- QEMU ----------
QEMU        ?= qemu-system-x86_64
# ধাপ ১২ক: ডিফল্ট 1 GiB (বেশি চাইলে: make run-serial QEMU_MEM=2G)
QEMU_MEM    ?= 1G
# ধাপ ১২ক: KVM (TCG emulation-এর চেয়ে অনেক গুণ দ্রুত; WSL2-তে: sudo usermod -aG kvm $USER,
# তারপর wsl --shutdown)। ধাপ ১৩: ডিফল্ট বন্ধ — KVM-এ rozOS-এর ভেতরে kernel বিল্ড করলে একটা
# জানা #GP crash আছে (claude/rozos-phase12a.md "Known issue"), ঠিক না হওয়া পর্যন্ত।
# চালু করতে: make run-serial KVM=1   (KVM=auto: /dev/kvm ব্যবহারযোগ্য হলে চালু)
KVM         ?= 0
ifeq ($(KVM),auto)
override KVM := $(shell [ -r /dev/kvm ] && [ -w /dev/kvm ] && echo 1 || echo 0)
endif
ifeq ($(KVM),1)
QEMU_ACCEL  ?= -accel kvm -cpu host
else
QEMU_ACCEL  ?=
endif
# ধাপ ১২: -no-reboot/-no-shutdown আর সাধারণ চালানোয় নেই — rozOS-এর `reboot` QEMU-র ভেতরেই
# restart করে (kinstall-এর পরে দরকার), `poweroff` QEMU বন্ধ করে। make debug-এ দুটোই থাকে
# (crash হলে QEMU থামে, বারবার restart নয়)।
QEMU_BASE_FLAGS ?= -M q35 -m $(QEMU_MEM)
QEMU_FLAGS  ?= $(QEMU_BASE_FLAGS) $(QEMU_ACCEL)
QEMU_DEBUG_FLAGS ?= -no-reboot -no-shutdown
# UEFI firmware (Ubuntu 24.04-এর ovmf প্যাকেজ)
OVMF_CODE   ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS   ?= /usr/share/OVMF/OVMF_VARS_4M.fd
