#!/usr/bin/env bash
# rozOS — x86_64-rozos টুলচেইন (ধাপ ১৭ক): binutils 2.46 + GCC 15.3 (C, C++)
#
# এটা "OS-specific" টার্গেট: GCC জানে rozOS-এর crt0/crti/crtn, rozlibc (-lc), static লিংক,
# __rozos__ ম্যাক্রো। ফলে  x86_64-rozos-g++ prog.cpp -o prog  সরাসরি rozOS-এর প্রোগ্রাম বানায়।
# (পুরোনো build-cross.sh-এর x86_64-elf কম্পাইলার কার্নেলের জন্য, সেটা বহাল।)
#
# ব্যবহার (rozOS ফোল্ডার থেকে, আগে  make CROSS= libc):
#   ./toolchain/build-rozos-toolchain.sh cross     PC-তে ক্রস টুলচেইন (~১–২ ঘণ্টা)
#   ./toolchain/build-rozos-toolchain.sh sysroot   শুধু sysroot হালনাগাদ (libc বদলালে)
#   ./toolchain/build-rozos-toolchain.sh native    rozOS-এর ভেতরে চলার binutils → rozpkg প্যাকেজ
#   ./toolchain/build-rozos-toolchain.sh native-gcc  rozOS-এর ভেতরে চলার GCC/G++ → gcc প্যাকেজ (.tar.gz)
#                                                  (ধাপ ১৭খ; GMP/MPFR/MPC-ও rozOS-এর জন্য বানায়, ~১ ঘণ্টা)
#   ./toolchain/build-rozos-toolchain.sh all       cross + native + native-gcc
#
# পরিবেশ চলক: PREFIX (ডিফল্ট ~/opt/rozos-cross), SYSROOT (~/opt/rozos-sysroot),
#             SRC_DIR (~/src/rozos-toolchain), JOBS (nproc), GCC_VERSION, BINUTILS_VERSION
set -euo pipefail

GCC_VERSION="${GCC_VERSION:-15.3.0}"
BINUTILS_VERSION="${BINUTILS_VERSION:-2.46.0}"
TARGET=x86_64-rozos
ROZOS="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/opt/rozos-cross}"
SYSROOT="${SYSROOT:-$HOME/opt/rozos-sysroot}"
SRC_DIR="${SRC_DIR:-$HOME/src/rozos-toolchain}"
JOBS="${JOBS:-$(nproc)}"
export PATH="$PREFIX/bin:$PATH"

log() { printf '\n\033[1;36m[rozos-toolchain]\033[0m %s\n' "$*" >&2; }   # stderr: $(unpack …)-এর ফলে মিশবে না
die() { printf '\033[1;31m[rozos-toolchain]\033[0m %s\n' "$*" >&2; exit 1; }

# একাধিক URL — প্রথম যেটা কাজ করে (gnu.org বন্ধ থাকলে Ubuntu-র archive)
fetch() {
    local out=$1; shift
    [ -s "$out" ] && { log "আগেই আছে: $out"; return 0; }
    for url in "$@"; do
        log "ডাউনলোড: $url"
        if curl -fL --retry 3 -o "$out.part" "$url"; then mv "$out.part" "$out"; return 0; fi
    done
    rm -f "$out.part"; return 1
}

# টারবল খুলে ভেতরের প্রথম ডিরেক্টরির নাম
unpack() {
    local tb=$1 dir
    dir=$(tar tf "$tb" | head -1 | cut -d/ -f1)
    [ -d "$dir" ] || { log "খোলা: $tb"; tar xf "$tb"; }
    echo "$SRC_DIR/$dir"
}

get_sources() {
    mkdir -p "$SRC_DIR"; cd "$SRC_DIR"
    local bv=${BINUTILS_VERSION%.0}          # 2.46.0 → 2.46 (Ubuntu-র নাম)
    fetch "binutils-$BINUTILS_VERSION.tar.xz" \
        "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
        "https://mirrors.kernel.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
        "http://archive.ubuntu.com/ubuntu/pool/main/b/binutils/binutils_$bv.orig.tar.xz" \
        || die "binutils-এর সোর্স পাওয়া গেল না"
    BINUTILS_SRC=$(unpack "binutils-$BINUTILS_VERSION.tar.xz")

    if fetch "gcc-$GCC_VERSION.tar.xz" \
        "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
        "https://mirrors.kernel.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz"; then
        GCC_SRC=$(unpack "gcc-$GCC_VERSION.tar.xz")
    else
        GCC_SRC="$SRC_DIR/gcc-$GCC_VERSION"
        [ -d "$GCC_SRC" ] || git clone --depth 1 --branch "releases/gcc-$GCC_VERSION" \
            https://github.com/gcc-mirror/gcc.git "$GCC_SRC" || die "GCC-এর সোর্স পাওয়া গেল না"
    fi
    log "rozOS টার্গেট প্যাচ"
    python3 "$ROZOS/toolchain/rozos-target-patch.py" "$BINUTILS_SRC" "$GCC_SRC"
}

# rozlibc-এর হেডার আর লাইব্রেরি → SYSROOT/usr/{include,lib}
sysroot() {
    local lb="$ROZOS/build/libc"
    [ -f "$lb/libc.a" ] || die "আগে rozOS ফোল্ডারে  make CROSS= libc  চালান"
    log "sysroot: $SYSROOT"
    mkdir -p "$SYSROOT/usr/include" "$SYSROOT/usr/lib"
    cp -rT "$ROZOS/libc/include" "$SYSROOT/usr/include"
    cp "$lb"/*.a "$lb"/crt0.o "$lb"/crti.o "$lb"/crtn.o "$SYSROOT/usr/lib/"
}

cross() {
    get_sources
    sysroot
    log "ক্রস binutils ($TARGET)"
    mkdir -p "$SRC_DIR/bld-binutils-cross"; cd "$SRC_DIR/bld-binutils-cross"
    [ -f Makefile ] || "$BINUTILS_SRC/configure" --target=$TARGET --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" --disable-nls --disable-werror --disable-gdb --disable-gdbserver \
        --disable-sim --disable-gprofng --disable-gold --enable-ld=default
    make -j"$JOBS" && make install

    log "ক্রস GCC ($TARGET, C/C++)"
    mkdir -p "$SRC_DIR/bld-gcc-cross"; cd "$SRC_DIR/bld-gcc-cross"
    # --disable-tls: rozOS-এ এখনো ELF TLS নেই → thread_local "emutls" দিয়ে চলে
    [ -f Makefile ] || "$GCC_SRC/configure" --target=$TARGET --prefix="$PREFIX" \
        --with-sysroot="$SYSROOT" --enable-languages=c,c++ --disable-nls --disable-multilib \
        --disable-shared --enable-threads=posix --disable-tls --enable-initfini-array \
        --disable-libssp --disable-libquadmath --disable-libgomp --disable-libsanitizer \
        --disable-libvtv --disable-libatomic --disable-libitm --enable-clocale=generic \
        --disable-bootstrap --disable-lto --disable-libstdcxx-pch --disable-werror \
        --without-isl --disable-libcc1 --disable-fixincludes
    make -j"$JOBS" all-gcc && make install-gcc
    make -j"$JOBS" all-target-libgcc && make install-target-libgcc
    make -j"$JOBS" all-target-libstdc++-v3 && make install-target-libstdc++-v3
    log "তৈরি: $PREFIX/bin/$TARGET-gcc, $TARGET-g++"
}

# rozOS-এর ভেতরে চলার binutils (Canadian cross: --host=$TARGET) → rozpkg প্যাকেজ
native() {
    command -v $TARGET-gcc >/dev/null || die "আগে  $0 cross"
    get_sources
    sysroot
    local stage="$SRC_DIR/stage-binutils" pkg="$SRC_DIR/pkg-binutils"
    log "rozOS-এর জন্য binutils (--host=$TARGET)"
    mkdir -p "$SRC_DIR/bld-binutils-native"; cd "$SRC_DIR/bld-binutils-native"
    [ -f Makefile ] || "$BINUTILS_SRC/configure" --host=$TARGET --target=$TARGET --prefix=/usr \
        --with-sysroot=/ --with-build-sysroot="$SYSROOT" --disable-nls --disable-werror \
        --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --disable-gold \
        --enable-ld=default --disable-plugins --disable-shared --enable-static \
        --without-zstd --without-debuginfod --disable-libctf
    make -j"$JOBS"
    rm -rf "$stage" "$pkg"; make install DESTDIR="$stage"
    # প্যাকেজে শুধু প্রোগ্রাম (stripped) আর ld-এর স্ক্রিপ্ট; ld.bfd = ld, তাই বাদ
    mkdir -p "$pkg/usr/bin" "$pkg/usr/x86_64-rozos/lib" "$pkg/usr/share/doc/binutils"
    for b in as ld ar nm objdump objcopy readelf strip size strings ranlib addr2line c++filt elfedit; do
        $TARGET-strip -o "$pkg/usr/bin/$b" "$stage/usr/bin/$b"
    done
    cp -r "$stage/usr/x86_64-rozos/lib/ldscripts" "$pkg/usr/x86_64-rozos/lib/"
    cp "$BINUTILS_SRC/COPYING3" "$pkg/usr/share/doc/binutils/" 2>/dev/null || true
    cp "$ROZOS/ports/packages/binutils-README.txt" "$pkg/usr/share/doc/binutils/README.txt"
    cd "$ROZOS"
    ports/packages/mkpkg.sh binutils "${BINUTILS_VERSION%.0}" "$pkg" \
        "GNU binutils (as, ld, ar, objdump ...) for rozOS"
}

# ধাপ ১৭খ: GCC-র গণিত লাইব্রেরি (GMP, MPFR, MPC) — rozOS-এর জন্য, static
hostlibs() {
    local H="$SRC_DIR/rozos-hostlibs" P=http://archive.ubuntu.com/ubuntu/pool/main
    [ -f "$H/lib/libmpc.a" ] && { HOSTLIBS=$H; return 0; }
    cd "$SRC_DIR"
    fetch gmp-6.3.0.tar.xz https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz \
        "$P/g/gmp/gmp_6.3.0+dfsg.orig.tar.xz" || die "GMP পাওয়া গেল না"
    fetch mpfr-4.2.2.tar.xz https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz \
        "$P/m/mpfr4/mpfr4_4.2.2.orig.tar.xz" || die "MPFR পাওয়া গেল না"
    fetch mpc-1.3.1.tar.gz https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz \
        "$P/m/mpclib3/mpclib3_1.3.1.orig.tar.gz" || die "MPC পাওয়া গেল না"
    local gmp mpfr mpc
    gmp=$(unpack gmp-6.3.0.tar.xz); mpfr=$(unpack mpfr-4.2.2.tar.xz); mpc=$(unpack mpc-1.3.1.tar.gz)
    # পুরোনো config.sub "rozos" চেনে না → GCC-র প্যাচ করা কপি (GMP-তে আসলটা configfsf.sub)
    cp "$GCC_SRC/config.sub" "$gmp/configfsf.sub"
    cp "$GCC_SRC/config.sub" "$mpfr/config.sub"; cp "$GCC_SRC/config.sub" "$mpc/build-aux/config.sub"
    # Ubuntu-র "+dfsg" টারবলে doc/ নেই — খালি Makefile.in
    [ -f "$gmp/doc/Makefile.in" ] || { mkdir -p "$gmp/doc"
        printf 'all install install-data install-exec check clean distclean uninstall:\n\t@:\n' > "$gmp/doc/Makefile.in"; }
    # CC স্পষ্ট দিতে হয় (নইলে GMP PC-র gcc নেয়); GCC 15-এর C23-তে GMP-র পরীক্ষা ভাঙে → gnu17
    local env=(CC=$TARGET-gcc CFLAGS="-O2 -std=gnu17" AR=$TARGET-ar RANLIB=$TARGET-ranlib NM=$TARGET-nm)
    local cf=(--host=$TARGET --build="$("$GCC_SRC/config.guess")" --prefix="$H" --disable-shared --enable-static)
    for lib in gmp mpfr mpc; do
        log "rozOS-এর জন্য $lib"
        rm -rf "$SRC_DIR/bld-$lib"; mkdir -p "$SRC_DIR/bld-$lib"; cd "$SRC_DIR/bld-$lib"
        case $lib in
            gmp)  env "${env[@]}" "$gmp/configure" "${cf[@]}" ;;
            mpfr) env "${env[@]}" "$mpfr/configure" "${cf[@]}" --with-gmp="$H" ;;
            mpc)  env "${env[@]}" "$mpc/configure" "${cf[@]}" --with-gmp="$H" --with-mpfr="$H" ;;
        esac
        make -j"$JOBS" && make install
    done
    HOSTLIBS=$H
}

# ধাপ ১৭খ: rozOS-এর ভেতরে চলার GCC (Canadian cross) → gcc প্যাকেজ
native_gcc() {
    command -v $TARGET-g++ >/dev/null || die "আগে  $0 cross"
    get_sources
    sysroot
    hostlibs
    local stage="$SRC_DIR/stage-gcc" pkg="$SRC_DIR/pkg-gcc" V=$GCC_VERSION T=$TARGET C="$PREFIX"
    log "rozOS-এর জন্য GCC (--host=$TARGET)"
    mkdir -p "$SRC_DIR/bld-gcc-native"; cd "$SRC_DIR/bld-gcc-native"
    [ -f Makefile ] || "$GCC_SRC/configure" --build="$("$GCC_SRC/config.guess")" --host=$T --target=$T \
        --prefix=/usr --with-build-sysroot="$SYSROOT" --with-native-system-header-dir=/usr/include \
        --with-gmp="$HOSTLIBS" --with-mpfr="$HOSTLIBS" --with-mpc="$HOSTLIBS" \
        --enable-languages=c,c++ --disable-nls --disable-multilib --disable-shared \
        --enable-threads=posix --disable-tls --enable-initfini-array --disable-libssp \
        --disable-libquadmath --disable-libgomp --disable-libsanitizer --disable-libvtv \
        --disable-libatomic --disable-libitm --enable-clocale=generic --disable-bootstrap \
        --disable-lto --disable-plugin --disable-libstdcxx-pch --disable-werror --without-isl \
        --disable-libcc1 --disable-fixincludes --without-zstd
    make -j"$JOBS" all-gcc
    rm -rf "$stage" "$pkg"; make install-gcc DESTDIR="$stage"
    # প্যাকেজ: ড্রাইভার + cc1/cc1plus (stripped), GCC-র হেডার; target লাইব্রেরি (libgcc,
    # crtbegin/end, libstdc++ আর তার হেডার) ক্রস টুলচেইন থেকে — একই টার্গেট, একই কনফিগারেশন
    mkdir -p "$pkg/usr/bin" "$pkg/usr/libexec/gcc/$T/$V" "$pkg/usr/lib/gcc/$T/$V" \
             "$pkg/usr/include/c++" "$pkg/usr/share/doc/gcc"
    for b in gcc g++ cpp; do $T-strip -o "$pkg/usr/bin/$b" "$stage/usr/bin/$b"; done
    cp "$pkg/usr/bin/g++" "$pkg/usr/bin/c++"              # rozpkg link চেনে না, তাই কপি
    for f in cc1 cc1plus collect2 lto-wrapper; do
        $T-strip -o "$pkg/usr/libexec/gcc/$T/$V/$f" "$stage/usr/libexec/gcc/$T/$V/$f"
    done
    cp -r "$C/lib/gcc/$T/$V/include" "$C/lib/gcc/$T/$V/include-fixed" "$pkg/usr/lib/gcc/$T/$V/"
    cp "$C/lib/gcc/$T/$V"/{crtbegin.o,crtend.o,libgcc.a,libgcov.a} "$pkg/usr/lib/gcc/$T/$V/"
    cp -r "$C/$T/include/c++/$V" "$pkg/usr/include/c++/"
    cp "$C/$T/lib/libstdc++.a" "$C/$T/lib/libstdc++exp.a" "$pkg/usr/lib/"
    $T-strip -g "$pkg/usr/lib/libstdc++.a" "$pkg/usr/lib/libstdc++exp.a" "$pkg/usr/lib/gcc/$T/$V/libgcc.a"
    cp "$GCC_SRC/COPYING3" "$GCC_SRC/COPYING.RUNTIME" "$pkg/usr/share/doc/gcc/"
    cp "$ROZOS/ports/packages/gcc-README.txt" "$pkg/usr/share/doc/gcc/README.txt"
    cd "$ROZOS"
    MKPKG_GZ=1 ports/packages/mkpkg.sh gcc "$V" "$pkg" \
        "GCC $V - C and C++ compiler (gcc, g++; needs binutils)"
}

case "${1:-}" in
    cross)   cross ;;
    sysroot) sysroot ;;
    native)  native ;;
    native-gcc) native_gcc ;;
    all)     cross; native; native_gcc ;;
    *) echo "usage: $0 cross|sysroot|native|native-gcc|all   (help toolchain)" >&2; exit 2 ;;
esac
