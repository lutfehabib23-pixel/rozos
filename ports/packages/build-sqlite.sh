#!/usr/bin/env bash
# rozOS — SQLite (https://sqlite.org, public domain) থেকে rozpkg প্যাকেজ "sqlite" (ধাপ ১৯ছ)
#
#   ./ports/packages/build-sqlite.sh        (rozOS রিপোর মূল ডিরেক্টরি থেকে; আগে `make`)
#
# GitHub-এর সরকারি কপি (sqlite/sqlite) থেকে নির্দিষ্ট ট্যাগ আনে, হোস্টে amalgamation
# (sqlite3.c + shell.c) বানায় — এর জন্য tclsh লাগে (sudo apt install tcl) — তারপর হোস্টের
# কম্পাইলার দিয়ে rozlibc-এর বিরুদ্ধে libsqlite3.a আর /bin/sqlite3 বানায়।
# ফল: dist/packages/sqlite-3.54.1.tar আর index.txt (mkpkg.sh)।
# rozOS-এ:  rozpkg install sqlite ;  sqlite3 my.db ;  tcc prog.c -lsqlite3
#
# rozOS-এর জন্য বাছাই: THREADSAFE=0 (এক থ্রেড থেকে ব্যবহার করুন), WAL নেই (ফাইলের shared
# mmap নেই — journal_mode DELETE চলে), extension লোড নেই (dlopen নেই)।
set -euo pipefail

VERSION=3.54.1
TAG=version-$VERSION
COMMIT=57520227c95396ce39ba6ecaac8d475b78dd3964    # ট্যাগের commit — বদলালে থামবে
URL=https://github.com/sqlite/sqlite.git
SRC=ports/packages/src/sqlite-$VERSION
WORK=build/packages/sqlite
CC=${CC:-${CROSS-x86_64-elf-}gcc}
AR=${AR:-ar}
STRIP=${STRIP:-strip}

[ -f libc/include/stdio.h ] || { echo "rozOS রিপোর মূল ডিরেক্টরি থেকে চালান" >&2; exit 1; }
[ -f build/libc/libc.a ] || { echo "আগে make CROSS= চালান (build/libc/libc.a লাগে)" >&2; exit 1; }
command -v "$CC" >/dev/null || { echo "$CC নেই — CROSS= দিলে হোস্টের gcc (যেমন: CROSS= $0)" >&2; exit 1; }
command -v tclsh >/dev/null || { echo "tclsh নেই — sudo apt install tcl" >&2; exit 1; }

if [ ! -f "$SRC/.rozos-commit" ] || [ "$(cat "$SRC/.rozos-commit")" != "$COMMIT" ]; then
    echo "  FETCH   $URL ($TAG)"
    rm -rf "$SRC"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" "$URL" "$SRC"
    GOT=$(git -C "$SRC" rev-parse HEAD)
    if [ "$GOT" != "$COMMIT" ]; then
        echo "SQLite commit মেলেনি: পাওয়া $GOT, আশা $COMMIT — থামানো হলো" >&2
        rm -rf "$SRC"
        exit 1
    fi
    echo "$COMMIT" > "$SRC/.rozos-commit"
fi

AMAL=$SRC/amalgamation
if [ ! -f "$AMAL/sqlite3.c" ] || [ ! -f "$AMAL/shell.c" ]; then
    echo "  GEN     sqlite3.c, shell.c (amalgamation — কয়েক মিনিট লাগতে পারে)"
    mkdir -p "$AMAL"
    ( cd "$AMAL" && ../configure --disable-tcl >/dev/null && make sqlite3.c shell.c >/dev/null )
fi

# rozOS-এর ইউজার-প্রোগ্রামের মতোই ফ্ল্যাগ (Makefile-এর USER_CFLAGS)
CFLAGS="-std=c99 -O2 -m64 -march=x86-64 -ffreestanding -nostdinc -isystem $($CC -print-file-name=include) \
 -Ilibc/include -fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables -w"
SQLFLAGS="-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL -DSQLITE_DEFAULT_MMAP_SIZE=0 \
 -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_OMIT_RANDOMNESS=0 -DHAVE_USLEEP=1 -DSQLITE_DQS=0 \
 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_DEFAULT_FOREIGN_KEYS=1"

rm -rf "$WORK" && mkdir -p "$WORK/obj"
ROOT="$WORK/root"
mkdir -p "$ROOT/usr/lib" "$ROOT/usr/include" "$ROOT/bin" "$ROOT/usr/share/doc/sqlite"
echo "  CC      sqlite3.c (বড় ফাইল — একটু সময় লাগে)"
$CC $CFLAGS $SQLFLAGS -c "$AMAL/sqlite3.c" -o "$WORK/obj/sqlite3.o"
$AR rcs "$ROOT/usr/lib/libsqlite3.a" "$WORK/obj/sqlite3.o"
"$STRIP" --strip-debug "$ROOT/usr/lib/libsqlite3.a"
echo "  CC      shell.c → /bin/sqlite3"
$CC $CFLAGS $SQLFLAGS -I"$AMAL" -c "$AMAL/shell.c" -o "$WORK/obj/shell.o"
$CC -nostdlib -static -no-pie -Wl,-z,max-page-size=0x1000 -Wl,--build-id=none -Wl,-Ttext-segment=0x400000 \
    build/libc/crt0.o "$WORK/obj/shell.o" "$WORK/obj/sqlite3.o" build/libc/libc.a -lgcc -o "$ROOT/bin/sqlite3"
"$STRIP" "$ROOT/bin/sqlite3"

cp "$AMAL/sqlite3.h" "$AMAL/sqlite3ext.h" "$ROOT/usr/include/"
cp "$SRC/LICENSE.md" "$ROOT/usr/share/doc/sqlite/" 2>/dev/null || true
cp ports/packages/sqlite-example.c "$ROOT/usr/share/doc/sqlite/example.c"
cat > "$ROOT/usr/share/doc/sqlite/README.rozos" <<TXT
SQLite $VERSION for rozOS (public domain, https://sqlite.org)
  sqlite3 FILE.db                  the SQL shell (.help, .tables, .schema, .quit)
  tcc prog.c -lsqlite3 -o prog     use it from C (#include <sqlite3.h>)
  example: tcc /usr/share/doc/sqlite/example.c -lsqlite3 -o /tmp/s && /tmp/s
Built with THREADSAFE=0 (use one thread), no WAL, no extension loading.
Help: help sqlite   C reference: cref sqlite3_open
TXT

./ports/packages/mkpkg.sh sqlite "$VERSION" "$ROOT" "SQLite $VERSION - SQL database in one file (sqlite3, tcc prog.c -lsqlite3)"

cat <<MSG

এখন dist/packages/-এর ফাইলগুলো GitHub Release-এ তুলুন:
    gh release upload packages -R YOUR-NAME/rozos-packages --clobber dist/packages/index.txt dist/packages/sqlite-$VERSION.tar
  (প্রথমবার হলে: gh repo create rozos-packages --public ;
   gh release create packages -R YOUR-NAME/rozos-packages --title "rozOS packages" dist/packages/*)
rozOS-এ:
    rozpkg update && rozpkg install sqlite
    sqlite3 /tmp/test.db
MSG
