GNU binutils 2.46 for rozOS (rozpkg package "binutils", phase 17a)
License: GPL-3.0-or-later (COPYING3 in this folder).
Source: https://ftp.gnu.org/gnu/binutils/  - built by
  ./toolchain/build-rozos-toolchain.sh native   (in the rozOS source tree)

Programs (in /usr/bin):
  as        GNU assembler (AT&T syntax, .s files)
  ld        GNU linker
  ar        make / list .a libraries      ranlib   index a .a
  nm        symbols of .o / .a            objdump  disassemble (-d), headers (-h)
  readelf   ELF headers (-h -l -S -s)     objcopy  copy / convert object files
  strip     remove symbols                size     section sizes
  strings   printable strings             addr2line  address -> file:line
  c++filt   demangle C++ names            elfedit  change ELF header fields
Note: /bin/strings (the small one) comes first in PATH; use /usr/bin/strings
for the GNU one.

Example - assemble and link by hand against rozlibc:
  as prog.s -o prog.o
  ld /usr/lib/crt0.o /usr/lib/crti.o prog.o -lc /usr/lib/crtn.o -o prog
See: help binutils
