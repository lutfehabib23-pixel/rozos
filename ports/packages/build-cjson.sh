#!/usr/bin/env bash
# rozOS — cJSON (https://github.com/DaveGamble/cJSON) থেকে rozpkg প্যাকেজ "cjson"
#
#   ./ports/packages/build-cjson.sh            (rozOS রিপোর মূল ডিরেক্টরি থেকে; আগে `make`)
#
# হোস্টের ক্রস-কম্পাইলার দিয়ে rozlibc-এর বিরুদ্ধে libcjson.a বানায় (cJSON + cJSON_Utils),
# তারপর mkpkg.sh দিয়ে dist/packages/cjson-1.7.18.tar + index.txt।
# rozOS-এ:  rozpkg install cjson ;  tcc prog.c -lcjson
set -euo pipefail

VERSION=1.7.18
COMMIT=acc76239bee01d8e9c858ae2cab296704e52d916     # v1.7.18 ট্যাগের commit — বদলালে থামবে
URL=https://github.com/DaveGamble/cJSON
SRC=ports/packages/src/cjson-$VERSION
WORK=build/packages/cjson
CC=${CC:-${CROSS-x86_64-elf-}gcc}
AR=${AR:-ar}

[ -f libc/include/stdio.h ] || { echo "rozOS রিপোর মূল ডিরেক্টরি থেকে চালান" >&2; exit 1; }
command -v "$CC" >/dev/null || { echo "$CC নেই — CROSS= দিলে হোস্টের gcc (যেমন: CROSS= $0)" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then
    echo "  FETCH   $URL (v$VERSION)"
    rm -rf "$SRC"
    git clone -q --depth 1 --branch "v$VERSION" "$URL" "$SRC"
fi
GOT=$(git -C "$SRC" rev-parse HEAD)
if [ "$GOT" != "$COMMIT" ]; then
    echo "cJSON commit মেলেনি: পাওয়া $GOT, আশা $COMMIT — থামানো হলো" >&2
    exit 1
fi

# rozOS-এর ইউজার-প্রোগ্রামের মতোই ফ্ল্যাগ (Makefile-এর USER_CFLAGS); -g ছাড়া, ছোট রাখতে
CFLAGS="-std=c99 -O2 -m64 -march=x86-64 -ffreestanding -nostdinc -isystem $($CC -print-file-name=include) \
 -Ilibc/include -fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables \
 -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types -w"

rm -rf "$WORK" && mkdir -p "$WORK/obj"
ROOT="$WORK/root"
mkdir -p "$ROOT/usr/lib" "$ROOT/usr/include/cjson" "$ROOT/usr/share/doc/cjson"
echo "  CC      cJSON.c cJSON_Utils.c"
$CC $CFLAGS -c "$SRC/cJSON.c" -o "$WORK/obj/cJSON.o"
$CC $CFLAGS -I"$SRC" -c "$SRC/cJSON_Utils.c" -o "$WORK/obj/cJSON_Utils.o"
$AR rcs "$ROOT/usr/lib/libcjson.a" "$WORK/obj/cJSON.o" "$WORK/obj/cJSON_Utils.o"

# হেডার: <cjson/cJSON.h> (upstream-এর নিয়ম) আর <cJSON.h> দুটোই চলে
cp "$SRC/cJSON.h" "$SRC/cJSON_Utils.h" "$ROOT/usr/include/cjson/"
cp "$SRC/cJSON.h" "$ROOT/usr/include/cJSON.h"
cp "$SRC/LICENSE" "$SRC/README.md" "$ROOT/usr/share/doc/cjson/"
cp ports/packages/cjson-example.c "$ROOT/usr/share/doc/cjson/example.c"

./ports/packages/mkpkg.sh cjson "$VERSION" "$ROOT" "cJSON $VERSION — ultralightweight JSON parser (tcc prog.c -lcjson)"

cat <<MSG

এখন dist/packages/-এর ফাইলগুলো GitHub Release-এ তুলুন (একবার রিপো বানাতে হয়):
    gh repo create rozos-packages --public
    gh release create packages -R YOUR-NAME/rozos-packages --title "rozOS packages" \\
        dist/packages/index.txt dist/packages/cjson-$VERSION.tar
  (পরে নতুন/হালনাগাদ প্যাকেজ:  gh release upload packages -R YOUR-NAME/rozos-packages --clobber dist/packages/*)
rozOS-এ:
    rozpkg repo YOUR-NAME/rozos-packages
    rozpkg update && rozpkg install cjson
    tcc /usr/share/doc/cjson/example.c -lcjson -o /tmp/j && /tmp/j
MSG
