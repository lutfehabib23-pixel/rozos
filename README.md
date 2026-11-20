# rozOS

**A 64-bit, self-hosting operating system for x86_64, written from scratch in C and assembly.**

rozOS boots with Limine (BIOS and UEFI) into its own kernel and a small graphical desktop. It includes a C compiler, a text editor, `make`, a TCP/IP stack with HTTPS, and a package manager, so you can write, compile and even rebuild the rozOS kernel **from inside rozOS itself**.

> Status: hobby / educational OS, under active development (current: **0.14.0-phase14a**).
> It runs in QEMU. Real hardware has not been tested yet.

---

## Features

| Area | What exists today |
|---|---|
| **Boot** | Limine, BIOS + UEFI, ISO and GPT disk image, boot menu with several kernels (`kinstall`) |
| **Kernel** | x86_64 long mode, GDT/IDT, PIC/PIT, PMM + VMM (4-level paging), kernel heap, preemptive multitasking, `fork`/`exec`/`wait`, Linux-like syscall ABI |
| **Processes** | process groups and sessions, signals (`kill`, SIG_IGN), Ctrl+C to the foreground job |
| **Filesystems** | VFS, ext2 (read/write root), FAT32 (boot partition), devfs, tmpfs, initrd |
| **IPC** | pipes, `poll`, shared memory (`shm_open` + `mmap MAP_SHARED`), PTYs, Unix domain sockets |
| **Drivers** | PCI, ACPI, PS/2 keyboard and mouse, serial, VGA/framebuffer console with PSF2 fonts, AHCI (SATA) disk, e1000/e1000e network |
| **Network** | lwIP TCP/IP, DHCP, DNS, BSD sockets, BearSSL TLS (HTTPS) |
| **libc** | own C library, *rozlibc* (stdio, stdlib, string, math, time, sockets, termios, …) |
| **Toolchain in the OS** | TinyCC (`tcc`), `make` (pdpmake), editor `edit` (syntax colours, Ctrl+R = compile and run) |
| **Packages** | `rozpkg`, which downloads C libraries over HTTPS and links them |
| **Graphics** | `/dev/fb0`, *librozgfx* (shapes, TrueType text via stb_truetype, BMP/PNG/JPEG via stb_image), *librozui* (3D-style widgets) |
| **Desktop** | **rozwm** compositing window manager: flicker-free damage-based drawing, movable/resizable windows with minimise/maximise/close, Start menu, taskbar, clock; **rozterm** terminal windows (VT100, scrollback, copy and paste) |
| **Shell** | `rozsh`: pipes, redirection, `&&`/`||`, job control, history; about 50 programs in `/bin` |
| **Docs** | 90 built-in help pages (`help`, `help desktop`, `help syscalls`, …) |

---

## Quick start

A Linux host is needed (Ubuntu/Debian, or WSL2 on Windows).

### 1. Install host tools
```sh
sudo apt install build-essential git python3 xorriso qemu-system-x86 ovmf \
                 e2fsprogs gdisk dosfstools mtools
```

### 2. Get the sources and third-party code
```sh
git clone https://github.com/<your-user>/rozOS.git
cd rozOS
./toolchain/get-limine.sh      # Limine bootloader + limine.h
./toolchain/get-tcc.sh         # TinyCC   (C compiler inside rozOS)
./toolchain/get-pdpmake.sh     # pdpmake  (make inside rozOS)
./toolchain/get-bearssl.sh     # BearSSL  (HTTPS)
make check-tools               # every line should say "ok"
```

### 3. Build and run
```sh
make CROSS=          # build with the host gcc (no cross compiler needed)
make run             # QEMU window → the rozOS desktop
```

Other targets:

| Command | Effect |
|---|---|
| `make run-uefi` | boot through UEFI (OVMF) |
| `make run-serial` | no window; you get a shell in this terminal |
| `make iso` / `make run-iso` | build or boot the live ISO |
| `make disk-reset` | recreate the disk image from scratch (**erases** files you made inside rozOS) |
| `make clean` | remove build output |

KVM is off by default (see *Known issues*). Turn it on with `make run KVM=1`. Memory is set with `QEMU_MEM=2G`.

### Inside rozOS
```
help            index of all help pages
systest         self-test (69 tests should pass)
edit hello.c    editor; Ctrl+R compiles and runs
tcc hello.c -o hello && ./hello
textmode        leave the desktop for the text console;  desktop  to go back
```

---

## Source tree

```
kernel/        the kernel
  arch/x86_64/   GDT, IDT, ISRs, paging, PIT, syscall entry
  mm/            physical/virtual memory, heap, shared memory
  proc/          processes, scheduler, ELF loader
  fs/            VFS, ext2, FAT, devfs, pipes
  drivers/       PS/2, serial, framebuffer/console, PTY, AHCI, e1000
  net/           lwIP port, BSD sockets, Unix sockets
  syscall/       system calls (Linux x86_64 numbers where possible)
libc/          rozlibc: headers + sources + crt0
userland/      init, shell, coreutils, editor, net tools, gfx apps, rozwm, rozterm, libraries
sysroot/       files copied as-is to the root disk (/etc, /usr/share, apps, fonts)
docs/help/     the built-in help pages (plain text, [[links]])
ports/         build glue for TinyCC, pdpmake, BearSSL, packages
toolchain/     host scripts: disk image, boot menu, fetchers, font converter
config.mk      build settings (CROSS, KVM, QEMU flags, memory)
```

---

## Contributing

Contributions of all sizes are welcome: bug reports, help pages, drivers, ported programs.

**Good places to start**
- **Desktop (phase 14b, in progress):** file manager, graphical code editor, more rozui widgets (text field, list, scroll bar, file dialog).
- **Drivers:** NVMe, USB (xHCI + HID + mass storage), more network cards.
- **Kernel:** user signal handlers + `SIGWINCH`, loadable kernel modules, SMP.
- **libc:** more POSIX coverage, eventually musl compatibility.
- **Toolchain:** binutils + GCC (C++) running inside rozOS.
- **Bug:** under KVM, the in-OS kernel build can hit a #GP at `iretq` (`isr.S`, error code 0x18).
- **Hardware testing:** boot the ISO on a real PC and report what happens.

**How to contribute**
1. Fork, create a branch (`git checkout -b feature/my-thing`).
2. Build with `make CROSS=`. Check that there are **no new warnings** and that `systest` still passes in both `make run` and `make run-uefi`.
3. If you add a user-visible feature, add or update a page in `docs/help/` (and a test in `userland/tests/systest.c` when possible).
4. Open a pull request that explains *what* and *why*.

**Code style**
- C99 (kernel freestanding, `-ffreestanding`), 4-space indent, braces on the same line.
- Keep the kernel self-contained: no host headers, no dynamic allocation in IRQ context.
- Do not remove existing features; extend them.
- Source comments are mostly in Bengali (বাংলা). English comments are equally welcome.

---

## Known issues
- KVM: a #GP fault during the in-OS kernel build (KVM is therefore off by default).
- No `SIGWINCH`; resize a terminal window, then press Ctrl+L in the editor.
- User signal handlers are not yet invoked (only default / ignore).
- `kmon` (kernel monitor) works only on the text console or serial line.

---

## Third-party components
| Component | Use | License |
|---|---|---|
| [Limine](https://github.com/Limine-Bootloader/Limine) | bootloader | BSD-2-Clause |
| [lwIP](https://savannah.nongnu.org/projects/lwip/) | TCP/IP stack (`kernel/net/lwip`) | BSD-3-Clause |
| [TinyCC](https://repo.or.cz/tinycc.git) | C compiler (fetched) | LGPL-2.1 |
| [pdpmake](https://frippery.org/make/) | make (fetched) | public domain |
| [BearSSL](https://bearssl.org) | TLS (fetched) | MIT |
| [stb_truetype / stb_image](https://github.com/nothings/stb) | fonts, images | MIT / public domain |
| [DejaVu fonts](https://dejavu-fonts.github.io) | UI and console fonts | Bitstream Vera / DejaVu license |

---

## বাংলায় সংক্ষেপে

rozOS একটি শূন্য থেকে বানানো ৬৪-বিট অপারেটিং সিস্টেম। এর ভেতরেই C কম্পাইলার, এডিটর, নেটওয়ার্ক আর প্যাকেজ ম্যানেজার আছে, তাই rozOS-এর ভেতর থেকেই rozOS-এর কোড লেখা ও বিল্ড করা যায়। বাংলা বা ইংরেজি যে কোনো ভাষায় issue আর pull request পাঠাতে পারেন। নতুন contributor-দের স্বাগতম!
