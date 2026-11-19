#!/usr/bin/env bash
# rozOS — x86_64-elf ক্রস-কম্পাইলার (binutils + GCC C/C++) বিল্ড স্ক্রিপ্ট
# ব্যবহার: ./toolchain/build-cross.sh
# ভার্সন বদলাতে: GCC_VERSION=16.2.0 ./toolchain/build-cross.sh
set -euo pipefail

BINUTILS_VERSION="${BINUTILS_VERSION:-2.46.0}"   # 2.46 থেকে টারবলের নামে ".0" যুক্ত হয়েছে
GCC_VERSION="${GCC_VERSION:-15.3.0}"
TARGET="x86_64-elf"
PREFIX="${PREFIX:-$HOME/opt/cross}"
SRC_DIR="${SRC_DIR:-$HOME/src/cross-build}"
JOBS="${JOBS:-$(nproc)}"

export PATH="$PREFIX/bin:$PATH"

log() { printf '\n\033[1;36m[rozOS-toolchain]\033[0m %s\n' "$*"; }

fetch() {
    # $1 = URL, $2 = ফাইলের নাম
    if [ ! -f "$2" ]; then
        log "ডাউনলোড: $1"
        curl -fL --retry 3 -o "$2.part" "$1"
        mv "$2.part" "$2"
    else
        log "আগেই আছে: $2"
    fi
}

mkdir -p "$SRC_DIR" "$PREFIX"
cd "$SRC_DIR"

# ---------- সোর্স ডাউনলোড ----------
fetch "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz" \
      "binutils-${BINUTILS_VERSION}.tar.xz"
fetch "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz" \
      "gcc-${GCC_VERSION}.tar.xz"

[ -d "binutils-${BINUTILS_VERSION}" ] || { log "binutils আনপ্যাক"; tar -xf "binutils-${BINUTILS_VERSION}.tar.xz"; }
[ -d "gcc-${GCC_VERSION}" ]           || { log "gcc আনপ্যাক";      tar -xf "gcc-${GCC_VERSION}.tar.xz"; }

# ---------- GCC প্যাচ: red-zone ছাড়া libgcc (multilib) ----------
# কার্নেল -mno-red-zone দিয়ে কম্পাইল হয়, তাই libgcc-এরও একটা no-red-zone ভার্সন দরকার।
# (-mcmodel=kernel দিয়ে libgcc বানানো যায় না, কারণ libgcc-র কিছু অংশ -fpic দিয়ে কম্পাইল হয়)
GCC_SRC="gcc-${GCC_VERSION}"
if [ ! -f "$GCC_SRC/gcc/config/i386/t-x86_64-elf" ]; then
    log "GCC-তে no-red-zone multilib প্যাচ যোগ হচ্ছে"
    cat > "$GCC_SRC/gcc/config/i386/t-x86_64-elf" <<'TMAKE'
MULTILIB_OPTIONS += mno-red-zone
MULTILIB_DIRNAMES += no-red-zone
TMAKE
fi
# শুধু x86_64-*-elf*) entry-র ঠিক পরের লাইনটা দেখা হয় (ফাইলের অন্য জায়গার মিল ধরা হয় না)
elf_entry_patched() {
    grep -A1 '^x86_64-\*-elf\*)' "$GCC_SRC/gcc/config.gcc" | grep -q 'i386/t-x86_64-elf'
}
if ! elf_entry_patched; then
    sed -i '/^x86_64-\*-elf\*)/a\	tmake_file="${tmake_file} i386/t-x86_64-elf"' "$GCC_SRC/gcc/config.gcc"
fi
elf_entry_patched \
    || { echo "config.gcc প্যাচ ব্যর্থ — x86_64-*-elf* entry পাওয়া যায়নি" >&2; exit 1; }

# ইনস্টল করা gcc-তে no-red-zone multilib আছে কিনা
has_multilib() {
    [ -x "$PREFIX/bin/${TARGET}-gcc" ] && \
    "$PREFIX/bin/${TARGET}-gcc" -print-multi-lib | grep -q 'no-red-zone'
}

# ---------- binutils ----------
if [ ! -x "$PREFIX/bin/${TARGET}-ld" ]; then
    log "binutils ${BINUTILS_VERSION} বিল্ড হচ্ছে"
    rm -rf build-binutils && mkdir build-binutils && cd build-binutils
    "../binutils-${BINUTILS_VERSION}/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --with-sysroot --disable-nls --disable-werror
    make -j"$JOBS"
    make install
    cd ..
else
    log "binutils আগেই ইনস্টল করা আছে"
fi

# ---------- GCC (C + C++) ----------
if ! has_multilib; then
    log "gcc ${GCC_VERSION} বিল্ড হচ্ছে (২০-৬০ মিনিট লাগতে পারে)"
    rm -rf build-gcc && mkdir build-gcc && cd build-gcc
    "../gcc-${GCC_VERSION}/configure" \
        --target="$TARGET" --prefix="$PREFIX" \
        --disable-nls --enable-languages=c,c++ --without-headers \
        --disable-hosted-libstdcxx
    make -j"$JOBS" all-gcc
    # libgcc: সাধারণ + no-red-zone (multilib প্যাচের কারণে দুটোই তৈরি হবে)
    make -j"$JOBS" all-target-libgcc
    make install-gcc
    make install-target-libgcc
    cd ..
else
    log "gcc আগেই ইনস্টল করা আছে (no-red-zone multilib সহ)"
fi
has_multilib || { echo "ত্রুটি: gcc-তে no-red-zone multilib তৈরি হয়নি" >&2; exit 1; }

# ---------- যাচাই ----------
log "যাচাই:"
"$PREFIX/bin/${TARGET}-gcc" --version | head -n1
"$PREFIX/bin/${TARGET}-g++" --version | head -n1
"$PREFIX/bin/${TARGET}-ld"  --version | head -n1
"$PREFIX/bin/${TARGET}-as"  --version | head -n1
echo "multilib: $("$PREFIX/bin/${TARGET}-gcc" -print-multi-lib | tr '\n' ' ')"
echo "no-red-zone libgcc: $("$PREFIX/bin/${TARGET}-gcc" -mno-red-zone -print-libgcc-file-name)"

log "সম্পন্ন। ~/.bashrc-এ এই লাইনটি আছে কিনা দেখুন:"
echo "    export PATH=\"$PREFIX/bin:\$PATH\""
