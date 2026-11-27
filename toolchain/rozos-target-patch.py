#!/usr/bin/env python3
# rozOS — binutils আর GCC-তে "x86_64-rozos" টার্গেট যোগ (ধাপ ১৭ক)
#
# ব্যবহার:  python3 toolchain/rozos-target-patch.py BINUTILS_SRC GCC_SRC
# বারবার চালানো নিরাপদ (যা আগে বসানো আছে তা আবার বসায় না)।
#
# কী করে:
#   config.sub (দুটোতেই)      — OS নাম "rozos" চেনানো
#   binutils: bfd/gas/ld      — ELF x86-64, ld-এর emulation elf_x86_64
#   gcc/config.gcc + rozos.h  — crt0/crti/crtbegin, -lc, static লিংক, __rozos__
#   libgcc/config.host        — crtbegin.o/crtend.o (C++ constructor, exception-এর frame নিবন্ধন)
#   fixincludes               — rozlibc-এর হেডার বদলানো হয় না
#   libstdc++-v3              — Linux-এর মতো link test দিয়ে libc-র ফাংশন খোঁজা
import os
import sys


def edit(path, old, new, marker):
    s = open(path).read()
    if marker in s:
        return
    if old not in s:
        sys.exit(f"rozos-target-patch: {path}: '{old.strip()[:60]}' পাওয়া যায়নি")
    s = s.replace(old, new, 1)
    open(path, "w").write(s)
    print(f"  patched {path}")


ROZOS_H = r'''/* rozOS টার্গেট (x86_64-rozos) — GCC-র জন্য OS-এর বিবরণ (ধাপ ১৭ক)
 * rozlibc: crt0.o (_start), crti.o/crtn.o (.init/.fini), libc.a। সব প্রোগ্রাম static,
 * ঠিকানা 0x400000 থেকে, পেজ ৪ KiB (rozOS-এর ELF লোডার)। */

#undef  TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()            \
  do {                                      \
    builtin_define ("__rozos__");           \
    builtin_define ("__unix__");            \
    builtin_define ("__unix");              \
    builtin_assert ("system=rozos");        \
    builtin_assert ("system=unix");         \
    builtin_assert ("system=posix");        \
  } while (0)

#undef  STARTFILE_SPEC
#define STARTFILE_SPEC "crt0.o%s crti.o%s crtbegin.o%s"

#undef  ENDFILE_SPEC
#define ENDFILE_SPEC "crtend.o%s crtn.o%s"

#undef  LIB_SPEC
#define LIB_SPEC "%{pthread:-lpthread} -lc"

#undef  LINK_SPEC
#define LINK_SPEC "-static -z max-page-size=0x1000 -z noexecstack %{!Ttext*:%{!Ttext-segment*:-Ttext-segment=0x400000}}"

/* প্রতিটা .s ফাইলের শেষে .note.GNU-stack — stack executable নয় */
#undef  TARGET_ASM_FILE_END
#define TARGET_ASM_FILE_END file_end_indicate_exec_stack

#undef  STANDARD_STARTFILE_PREFIX
#define STANDARD_STARTFILE_PREFIX "/usr/lib/"

/* -pthread: _REENTRANT আর -lpthread (rozos.opt-এ অপশনটা) */
#undef  CPP_SPEC
#define CPP_SPEC "%{pthread:-D_REENTRANT}"
'''

ROZOS_OPT = r'''; rozOS টার্গেটের ড্রাইভার অপশন (ধাপ ১৭ক)

pthread
Driver

posix
Driver

rdynamic
Driver
'''

# GCC-র বিল্ড প্রতিটা .opt-এর পাশে .opt.urls চায় (ডকুমেন্টেশনের লিংক)
ROZOS_OPT_URLS = r'''; rozOS: rozos.opt-এর ডকুমেন্টেশন লিংক (gnu-user.opt.urls-এর মতো)

rdynamic
UrlSuffix(gcc/Link-Options.html#index-rdynamic)
'''


def patch_config_sub(path):
    s = open(path).read()
    if "rozos*" in s:
        return
    if "\t| redox* \\\n" in s:                       # নতুন ধাঁচ (এক লাইনে একটা)
        s = s.replace("\t| redox* \\\n", "\t| redox* \\\n\t| rozos* \\\n", 1)
    elif "| redox* |" in s:                          # পুরোনো ধাঁচ
        s = s.replace("| redox* |", "| redox* | rozos* |", 1)
    else:
        sys.exit(f"rozos-target-patch: {path}: OS তালিকা পাওয়া যায়নি")
    open(path, "w").write(s)
    print(f"  patched {path}")


def binutils(src):
    patch_config_sub(f"{src}/config.sub")
    edit(f"{src}/bfd/config.bfd", "  x86_64-*-redox*)\n",
         "  x86_64-*-rozos*)\n    targ_defvec=x86_64_elf64_vec\n    targ_selvecs=i386_elf32_vec\n"
         "    want64=true\n    ;;\n  x86_64-*-redox*)\n", "x86_64-*-rozos*")
    edit(f"{src}/gas/configure.tgt", "  i386-*-redox*)\t\t\tfmt=elf ;;\n",
         "  i386-*-redox*)\t\t\tfmt=elf ;;\n  i386-*-rozos*)\t\t\tfmt=elf ;;\n", "i386-*-rozos*")
    edit(f"{src}/ld/configure.tgt", "x86_64-*-redox*)\ttarg_emul=elf_x86_64\n",
         "x86_64-*-rozos*)\ttarg_emul=elf_x86_64\n\t\t\ttarg_extra_emuls=elf_i386\n\t\t\t;;\n"
         "x86_64-*-redox*)\ttarg_emul=elf_x86_64\n", "x86_64-*-rozos*")


def gcc(src):
    patch_config_sub(f"{src}/config.sub")
    edit(f"{src}/gcc/config.gcc", "*-*-elf|arc*-*-elf*)\n",
         "*-*-rozos*)\n  # ধাপ ১৭ক: rozOS (rozlibc, static, Linux-এর syscall নম্বর)\n  gas=yes\n  gnu_ld=yes\n"
         "  default_use_cxa_atexit=yes\n  use_gcc_stdint=wrap\n  extra_options=\"${extra_options} rozos.opt\"\n  ;;\n*-*-elf|arc*-*-elf*)\n", "*-*-rozos*)")
    # আগে-প্যাচ করা গাছে -pthread ইত্যাদি অপশন (rozos.opt)
    edit(f"{src}/gcc/config.gcc", "  default_use_cxa_atexit=yes\n  use_gcc_stdint=wrap\n  ;;\n*-*-elf|arc*-*-elf*)\n",
         "  default_use_cxa_atexit=yes\n  use_gcc_stdint=wrap\n  extra_options=\"${extra_options} rozos.opt\"\n  ;;\n*-*-elf|arc*-*-elf*)\n", "rozos.opt")
    for path, text in ((f"{src}/gcc/config/rozos.opt", ROZOS_OPT),
                       (f"{src}/gcc/config/rozos.opt.urls", ROZOS_OPT_URLS)):
        if not os.path.exists(path) or open(path).read() != text:
            open(path, "w").write(text)
            print(f"  wrote {path}")
    edit(f"{src}/gcc/config.gcc", "x86_64-*-elf*)\n\ttm_file=",
         "x86_64-*-rozos*)\n\ttm_file=\"${tm_file} i386/unix.h i386/att.h elfos.h glibc-stdint.h i386/i386elf.h "
         "i386/x86-64.h rozos.h\"\n\t;;\nx86_64-*-elf*)\n\ttm_file=", "x86_64-*-rozos*)\n\ttm_file")
    path = f"{src}/gcc/config/rozos.h"
    if not os.path.exists(path) or open(path).read() != ROZOS_H:
        open(path, "w").write(ROZOS_H)
        print(f"  wrote {path}")
    edit(f"{src}/libgcc/config.host", "x86_64-*-elf* | x86_64-*-rtems*)\n",
         "x86_64-*-rozos*)\n\ttmake_file=\"$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic\"\n"
         "\textra_parts=\"crtbegin.o crtend.o\"\n\t;;\nx86_64-*-elf* | x86_64-*-rtems*)\n", "x86_64-*-rozos*")
    edit(f"{src}/fixincludes/mkfixinc.sh", "    *-musl* )\n", "    *-musl* | \\\n    *-rozos* )\n", "*-rozos*")
    for f in ("libstdc++-v3/crossconfig.m4", "libstdc++-v3/configure"):
        edit(f"{src}/{f}", "  *-linux* | *-uclinux* | *-gnu* | *-kfreebsd*-gnu | *-cygwin* | *-solaris*)\n",
             "  *-linux* | *-uclinux* | *-gnu* | *-kfreebsd*-gnu | *-cygwin* | *-solaris* | *-rozos*)\n", "*-rozos*)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: rozos-target-patch.py BINUTILS_SRC GCC_SRC")
    binutils(sys.argv[1])
    gcc(sys.argv[2])
