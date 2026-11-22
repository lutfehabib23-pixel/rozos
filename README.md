# rozOS

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**A 64-bit, self-hosting operating system for x86_64, written from scratch in C and assembly.**

rozOS boots with Limine (BIOS and UEFI) into its own kernel and a small graphical desktop. It includes a C compiler, a text editor, `make`, a TCP/IP stack with HTTPS, and a package manager, so you can write, compile and even rebuild the rozOS kernel **from inside rozOS itself**.

> Status: hobby / educational OS, under active development (current: **0.18.1-phase18b**).
> It runs in QEMU. Real hardware has not been tested yet.

---

## Features

| Area | What exists today |
|---|---|
| **Boot** | Limine, BIOS + UEFI, ISO and GPT disk image, boot menu with several kernels (`kinstall`) |
| **Kernel** | x86_64 long mode, GDT/IDT, PIC/PIT, PMM + VMM (4-level paging), kernel heap, preemptive multitasking, threads (`clone`, `futex`, TLS), `fork`/`exec`/`wait`, Linux-like syscall ABI |
| **Processes** | process groups and sessions, Ctrl+C to the foreground job, real signal handlers (`sigaction`, SA_SIGINFO, SA_RESTART, masks, `sigsuspend`, `sigaltstack`, `alarm`/`setitimer`, SIGCHLD, SIGWINCH, catching SIGSEGV/SIGFPE) |
| **Filesystems** | VFS, ext2 (read/write root), FAT32 (boot partition), devfs, tmpfs, initrd; `rename()`, `mount`/`umount` |
| **IPC** | pipes, `poll`, shared memory (`shm_open` + `mmap MAP_SHARED`), PTYs, Unix domain sockets |
| **Drivers** | PCI, ACPI, PS/2 keyboard and mouse, serial, VGA/framebuffer console with PSF2 fonts, AHCI (SATA) and NVMe disks, USB (xHCI: pen drives, keyboards, mice, hot-plug), e1000/e1000e network |
| **Network** | lwIP TCP/IP, DHCP, DNS, BSD sockets, BearSSL TLS (HTTPS) |
| **libc** | own C library, *rozlibc*: most of POSIX 2008 + C99, pthread — exact printf, wchar/UTF-8, locale, iconv, regex, getopt_long, time zones, full libm, select/readv; Lua and zlib build unchanged with the in-OS tcc |
| **Kernel modules** | `insmod`/`lsmod`/`rmmod`: ELF relocatable `.ko` files (gcc or the in-OS tcc), exported-symbol table, example `/lib/modules/hello.ko` |
| **Toolchain in the OS** | TinyCC (`tcc`), `make` (pdpmake), Code (graphical) and `edit` (terminal, F10 menu) editors, Lua 5.4 (`lua`, `liblua.a`), zlib (`gzip`/`gunzip`, `libz.a`) |
| **Packages** | `rozpkg`, which downloads C libraries over HTTPS and links them |
| **Graphics** | `/dev/fb0`, *librozgfx* (shapes, TrueType text via stb_truetype, BMP/PNG/JPEG via stb_image) |
| **Games** | *librozgame*: fixed-step game loop, real key press/release (`/dev/keyev`), fast image drawing (row-classified sprites, memcpy for opaque rows), sprite sheets and animation, scaling (fit / integer / native, optional bilinear), high-quality photo resize, collisions, TrueType text, menus, best scores, camera (follow, bounds, shake), text-file tile maps with move-and-slide, particles; Lua games with `luagame`; **Snake**, **Breakout**, **Tetris**, **Platformer** (own levels as `.map` text files) and **Star catcher** (Lua), with source in `/usr/src/games` and starter templates (`edit --new game`) |
| **Widgets** | *librozui*: immediate-mode buttons, check boxes, text boxes, lists, scroll bars, menu bars with shortcuts, message boxes, input and file dialogs; icons can be replaced by pictures |
| **Desktop** | **rozwm** compositing window manager: flicker-free damage-based drawing, movable/resizable windows with minimise/maximise/close, Start menu, taskbar, clock; **rozterm** terminal windows (VT100, scrollback, copy and paste, blinking cursor); dark 3D theme (Ebony, Obsidian, Onyx, Jet Black) |
| **Desktop apps** | **Code** (graphical C editor: menus, syntax colours, compile and run with tcc, clickable errors), **Files** (file manager), calculator, paint, image viewer |
| **Shell** | `rozsh`: pipes, redirection, `&&`/`||`, job control, history; about 50 programs in `/bin` |
| **Docs** | 107 built-in help pages (`help`, `help desktop`, `help syscalls`, …) |

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
git clone https://github.com/lutfehabib23-pixel/rozos.git rozOS
cd rozOS
./toolchain/get-limine.sh      # Limine bootloader + limine.h
./toolchain/get-tcc.sh         # TinyCC   (C compiler inside rozOS)
./toolchain/get-pdpmake.sh     # pdpmake  (make inside rozOS)
./toolchain/get-bearssl.sh     # BearSSL  (HTTPS)
./toolchain/get-lua.sh         # Lua 5.4  (/bin/lua, liblua)
./toolchain/get-zlib.sh        # zlib     (gzip, gunzip, libz)
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
| `make run USB=1` | also attach USB: a 64 MiB pen drive (mounted on `/mnt/usb`), keyboard, mouse; `USB_DEV=/dev/sdX` = a real pen drive via usbipd |
| `make run NVME=1` | also attach a 64 MiB NVMe SSD (`mount /dev/nvmea /mnt/nvme`) |
| `make disk-reset` | recreate the disk image from scratch (**erases** files you made inside rozOS) |
| `make clean` | remove build output |

KVM is off by default (see *Known issues*). Turn it on with `make run KVM=1`. Memory is set with `QEMU_MEM=2G`.

### Inside rozOS
```
help            index of all help pages
systest         self-test (191 checks should pass)
snake, breakout, tetris, platformer   games (Start > Games); help rozgame to write your own
luagame /usr/share/games/stars.lua    a game in Lua (help luagame)
lsusb           USB devices; pen drives appear on /mnt/usb
code hello.c    graphical editor (F5 runs);  edit hello.c in a terminal
files           file manager
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
userland/      init, shell, coreutils, editor, net tools, gfx apps, games, rozwm, rozterm, libraries
sysroot/       files copied as-is to the root disk (/etc, /usr/share, apps, fonts)
modules/       example kernel modules (built into /lib/modules/*.ko)
docs/help/     the built-in help pages (plain text, [[links]])
ports/         build glue for TinyCC, pdpmake, BearSSL, packages
toolchain/     host scripts: disk image, boot menu, fetchers, font converter, game art
config.mk      build settings (CROSS, KVM, QEMU flags, memory)
```

---

## Contributing

Contributions of all sizes are welcome: bug reports, help pages, drivers, ported programs.

**Good places to start**
- **Desktop:** more apps (settings, text viewer, games), drag and drop in Files, Bengali text shaping, icon pictures (`/usr/share/icons`).
- **Drivers:** USB hubs, more network cards, interrupt-driven xHCI/NVMe/AHCI, NTFS/exFAT reading.
- **Kernel:** SMP (use all CPU cores), job control (Ctrl+Z, `fg`/`bg`), more exported symbols for modules.
- **libc:** shared libraries, per-thread signal masks, eventually musl compatibility.
- **Toolchain:** binutils + GCC (C++) running inside rozOS.
- **Bug:** under KVM, the in-OS kernel build hit a #GP at `iretq` (error 0x18). It is now caught and logged - help find the root cause.
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
- KVM: an `iretq` #GP fault was seen during the in-OS kernel build. Since 0.15 it is caught and logged (and a damaged GDT is repaired) instead of panicking; KVM stays off by default until confirmed.
- No `SIGWINCH`; resize a terminal window, then press Ctrl+L in the editor.
- User signal handlers are not yet invoked (only default / ignore).
- `kmon` (kernel monitor) works only on the text console or serial line.
- The screen fonts have no Bengali glyphs yet (UTF-8 text is kept, shown as dots).

---

## Safety on real hardware
rozOS has not been tested much on real PCs yet. To protect your data, on a real computer every internal disk without a rozOS partition (for example your Windows SSD) is **read-only**; add `diskwrite` to the kernel command line to change that.

## License
rozOS is released under the [MIT License](LICENSE). Third-party components keep their own licenses (below).

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
