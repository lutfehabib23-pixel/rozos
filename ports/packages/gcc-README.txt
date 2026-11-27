GCC 15.3.0 for rozOS (rozpkg package "gcc", phase 17b)
License: GPL-3.0-or-later with the GCC Runtime Library Exception
(COPYING3, COPYING.RUNTIME in this folder): programs you build may use any
license. Source: https://gcc.gnu.org - built by
  ./toolchain/build-rozos-toolchain.sh native-gcc   (in the rozOS source tree)

Needs the "binutils" package (as, ld):  rozpkg install binutils gcc

  gcc prog.c -o prog              C (C23 by default; -std=c99 etc.)
  g++ prog.cpp -o prog            C++ (C++17 by default; -std=c++20/23)
  g++ -O2 -pthread t.cpp -o t     std::thread
  gcc -S prog.c                   look at the assembly (prog.s)

Files: /usr/bin/{gcc,g++,c++,cpp}, /usr/libexec/gcc/x86_64-rozos/15.3.0/
(cc1, cc1plus - the real compilers), /usr/lib/gcc/x86_64-rozos/15.3.0/
(libgcc, crtbegin/crtend, GCC's headers), /usr/include/c++/15.3.0 and
/usr/lib/libstdc++.a (the C++ library).
See: help gcc
