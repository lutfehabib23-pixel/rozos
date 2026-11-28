#!/usr/bin/env bash
# rozOS — rozpdf (PDF রিডার) থেকে rozpkg প্যাকেজ "rozpdf" (ধাপ ২৬খ-১)
#
#   ./ports/packages/build-rozpdf.sh        (rozOS রিপোর মূল ডিরেক্টরি থেকে; আগে `make CROSS=`)
#
# rozOS-এর ইউজার-প্রোগ্রামের মতোই বানায় (একই কম্পাইলার আর ফ্ল্যাগ; librozui + librozgfx —
# এর ভেতরেই FreeType — আর zlib)। PDF-এর Type1 ফন্টের জন্য FreeType-এর type1 module আলাদা
# কম্পাইল করে প্রোগ্রামেই জোড়া হয় (rozOS-এর librozgfx বদলায় না)।
# সাথে সোর্স (/usr/src/rozpdf — rozOS-এর ভেতরে `make` দিলে TCC দিয়ে, Type1 module ছাড়া),
# Start মেনুর এন্ট্রি, help পাতা আর একটা নমুনা PDF।
# ফল: dist/packages/rozpdf-VERSION.tar.gz আর index.txt (mkpkg.sh)।
set -euo pipefail

VERSION=1.0
CC=${CC:-${CROSS-x86_64-elf-}gcc}
STRIP=${STRIP:-strip}
WORK=build/packages/rozpdf
SRC=ports/rozpdf
FT=ports/freetype/src

[ -f libc/include/stdio.h ] || { echo "rozOS রিপোর মূল ডিরেক্টরি থেকে চালান" >&2; exit 1; }
for f in build/libc/libc.a build/libc/crt0.o build/userland/lib/librozui.a build/userland/lib/librozgfx.a \
         build/devlib/libz.a build/zlib/include/zconf.h "$FT/include/ft2build.h"; do
    [ -f "$f" ] || { echo "আগে make CROSS= চালান ($f লাগে)" >&2; exit 1; }
done
command -v "$CC" >/dev/null || { echo "$CC নেই — CROSS= দিলে হোস্টের gcc (যেমন: CROSS= $0)" >&2; exit 1; }

# rozOS-এর ইউজার-প্রোগ্রামের মতোই ফ্ল্যাগ (Makefile-এর USER_CFLAGS)
CFLAGS="-std=c99 -O2 -m64 -march=x86-64 -ffreestanding -nostdinc -isystem $($CC -print-file-name=include) \
 -Ilibc/include -Iuserland/lib -fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables"
FTFLAGS="-I$FT/include -Iports/freetype/rozos"
ZFLAGS="-Ibuild/zlib/include -Iports/zlib/src"

rm -rf "$WORK" && mkdir -p "$WORK/obj"
ROOT="$WORK/root"
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/src/rozpdf" "$ROOT/usr/share/apps" "$ROOT/usr/share/help" "$ROOT/usr/share/doc/rozpdf"
for f in pdfparse pdfrender rozpdf; do
    echo "  CC      $f.c"
    $CC $CFLAGS $FTFLAGS $ZFLAGS -DROZPDF_TYPE1 -c "$SRC/$f.c" -o "$WORK/obj/$f.o"
done
echo "  CC      FreeType type1 module"
$CC $CFLAGS -w -DFT2_BUILD_LIBRARY $FTFLAGS '-DFT_CONFIG_MODULES_H=<rozft-module.h>' '-DFT_CONFIG_OPTIONS_H=<rozft-option.h>' \
    -c "$FT/src/type1/type1.c" -o "$WORK/obj/type1.o"
$CC -nostdlib -static -no-pie -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -Wl,-Ttext-segment=0x400000 \
    build/libc/crt0.o "$WORK/obj/rozpdf.o" "$WORK/obj/pdfparse.o" "$WORK/obj/pdfrender.o" "$WORK/obj/type1.o" \
    build/userland/lib/librozui.a build/userland/lib/librozgfx.a build/devlib/libz.a \
    build/libc/libm.a build/libc/libc.a -lgcc -o "$ROOT/usr/bin/rozpdf"
"$STRIP" "$ROOT/usr/bin/rozpdf"

cp "$SRC/rozpdf.c" "$SRC/pdfparse.c" "$SRC/pdfrender.c" "$SRC/pdf.h" "$SRC/Makefile" "$ROOT/usr/src/rozpdf/"
mkdir -p "$ROOT/usr/src/rozpdf/ft"
cp -r "$FT/include/." "$ROOT/usr/src/rozpdf/ft/"            # FreeType-এর হেডার (rozOS-এর ভেতরে বানাতে)
cp ports/freetype/rozos/*.h "$ROOT/usr/src/rozpdf/ft/"
cp "$FT/LICENSE.TXT" "$ROOT/usr/src/rozpdf/ft/FreeType-LICENSE.TXT" 2>/dev/null || true
cp "$SRC/rozpdf.app" "$ROOT/usr/share/apps/rozpdf.app"
cp "$SRC/rozpdf-help.txt" "$ROOT/usr/share/help/rozpdf.txt"
cp "$SRC/sample.pdf" "$ROOT/usr/share/doc/rozpdf/sample.pdf"
cat > "$ROOT/usr/share/doc/rozpdf/README.txt" <<TXT
rozpdf $VERSION - the PDF reader for rozOS (phase 26b-1)
  rozpdf [FILE.pdf]            or Start > Office > rozpdf, or double-click a .pdf in Files
  rozpdf -t FILE.pdf           print the text      rozpdf -i FILE.pdf   information
  rozpdf -r FILE.pdf 1 p.png   save page 1 as a picture
  help rozpdf                  keys and features
  sample: rozpdf /usr/share/doc/rozpdf/sample.pdf
Source: /usr/src/rozpdf (make = TCC, make CC=gcc, make install)
TXT

MKPKG_GZ=1 ports/packages/mkpkg.sh rozpdf "$VERSION" "$ROOT" \
    "rozpdf $VERSION - PDF reader: text with the PDF's fonts, Bangla, pictures, find, copy text"
