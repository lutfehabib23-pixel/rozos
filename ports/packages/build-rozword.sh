#!/usr/bin/env bash
# rozOS — rozword (word processor) থেকে rozpkg প্যাকেজ "rozword" (ধাপ ২৬ক)
#
#   ./ports/packages/build-rozword.sh        (rozOS রিপোর মূল ডিরেক্টরি থেকে; আগে `make CROSS=`)
#
# rozOS-এর ইউজার-প্রোগ্রামের মতোই বানায় (একই কম্পাইলার আর ফ্ল্যাগ, librozui + librozgfx),
# সাথে সোর্স (/usr/src/rozword — rozOS-এর ভেতরে `make` দিলে TCC দিয়ে আবার বানানো যায়),
# Start মেনুর এন্ট্রি, help পাতা আর একটা নমুনা দলিল।
# ফল: dist/packages/rozword-VERSION.tar.gz আর index.txt (mkpkg.sh)।
set -euo pipefail

VERSION=1.1
CC=${CC:-${CROSS-x86_64-elf-}gcc}
STRIP=${STRIP:-strip}
WORK=build/packages/rozword
SRC=ports/rozword

[ -f libc/include/stdio.h ] || { echo "rozOS রিপোর মূল ডিরেক্টরি থেকে চালান" >&2; exit 1; }
for f in build/libc/libc.a build/libc/crt0.o build/userland/lib/librozui.a build/userland/lib/librozgfx.a; do
    [ -f "$f" ] || { echo "আগে make CROSS= চালান ($f লাগে)" >&2; exit 1; }
done
command -v "$CC" >/dev/null || { echo "$CC নেই — CROSS= দিলে হোস্টের gcc (যেমন: CROSS= $0)" >&2; exit 1; }

# rozOS-এর ইউজার-প্রোগ্রামের মতোই ফ্ল্যাগ (Makefile-এর USER_CFLAGS)
CFLAGS="-std=c99 -O2 -m64 -march=x86-64 -ffreestanding -nostdinc -isystem $($CC -print-file-name=include) \
 -Ilibc/include -Iuserland/lib -fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables"

rm -rf "$WORK" && mkdir -p "$WORK/obj"
ROOT="$WORK/root"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/src/rozword" "$ROOT/usr/share/apps" "$ROOT/usr/share/help" "$ROOT/usr/share/doc/rozword"
echo "  CC      rozword.c"
$CC $CFLAGS -c "$SRC/rozword.c" -o "$WORK/obj/rozword.o"
$CC -nostdlib -static -no-pie -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -Wl,-Ttext-segment=0x400000 \
    build/libc/crt0.o "$WORK/obj/rozword.o" build/userland/lib/librozui.a build/userland/lib/librozgfx.a \
    build/libc/libm.a build/libc/libc.a -lgcc -o "$ROOT/usr/bin/rozword"
"$STRIP" "$ROOT/usr/bin/rozword"

cp "$SRC/rozword.c" "$SRC/Makefile" "$ROOT/usr/src/rozword/"
cp "$SRC/rozword.app" "$ROOT/usr/share/apps/rozword.app"
cp "$SRC/rozword-help.txt" "$ROOT/usr/share/help/rozword.txt"
cp "$SRC/welcome.rwd" "$ROOT/usr/share/doc/rozword/welcome.rwd"
cat > "$ROOT/usr/share/doc/rozword/README.txt" <<TXT
rozword $VERSION - the word processor for rozOS (phase 26a)
  rozword [FILE.rwd | FILE.txt]     or Start > Office > rozword
  help rozword                      keys and features
  sample: rozword /usr/share/doc/rozword/welcome.rwd
Source: /usr/src/rozword (make = TCC, make CC=gcc, make install)
TXT

MKPKG_GZ=1 ports/packages/mkpkg.sh rozword "$VERSION" "$ROOT" \
    "rozword $VERSION - word processor: fonts, headings, lists, tables, pictures, header/footer, Bangla (.rwd, .txt)"
