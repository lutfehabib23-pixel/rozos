#!/usr/bin/env bash
# rozOS — একটা প্যাকেজ-ডিরেক্টরি থেকে rozpkg প্যাকেজ (.tar) বানানো আর index.txt হালনাগাদ
#
#   ports/packages/mkpkg.sh NAME VERSION PKGROOT "description" [OUTDIR]
#
# PKGROOT-এর ভেতরের পথ = rozOS-এ বসার পথ (যেমন PKGROOT/usr/lib/libfoo.a → /usr/lib/libfoo.a)।
# ফল: OUTDIR/NAME-VERSION.tar আর OUTDIR/index.txt (একই নামের পুরোনো লাইন বদলে যায়)।
# OUTDIR ডিফল্ট dist/packages। তারপর OUTDIR-এর সব ফাইল GitHub Release-এ তুলুন (help rozpkg)।
# ধাপ ১৭খ: MKPKG_GZ=1 দিলে NAME-VERSION.tar.gz (gzip -9 -n; rozOS ১৭খ-এর rozpkg লাগে)।
set -euo pipefail

if [ $# -lt 4 ]; then
    echo "usage: $0 NAME VERSION PKGROOT \"description\" [OUTDIR]" >&2
    exit 2
fi
NAME=$1 VERSION=$2 PKGROOT=$3 DESC=$4 OUTDIR=${5:-dist/packages}

if ! [[ "$NAME" =~ ^[a-z0-9][a-z0-9._+-]*$ ]]; then
    echo "mkpkg: নাম শুধু ছোট হাতের অক্ষর, সংখ্যা আর . _ + - হতে পারে" >&2
    exit 2
fi
[ -d "$PKGROOT" ] || { echo "mkpkg: $PKGROOT নেই" >&2; exit 1; }
mkdir -p "$OUTDIR"
FILE="$NAME-$VERSION.tar"
[ "${MKPKG_GZ:-0}" = 1 ] && FILE="$FILE.gz"

# USTAR (rozpkg শুধু এটাই পড়ে), মালিক root, স্থির ক্রম আর সময় — একই ইনপুটে একই sha256
if [ "${MKPKG_GZ:-0}" = 1 ]; then
    tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name \
        --mtime='2026-01-01 00:00:00Z' -C "$PKGROOT" -cf - . | gzip -9 -n > "$OUTDIR/$FILE"
else
    tar --format=ustar --owner=0 --group=0 --numeric-owner --sort=name \
        --mtime='2026-01-01 00:00:00Z' -C "$PKGROOT" -cf "$OUTDIR/$FILE" .
fi

SHA=$(sha256sum "$OUTDIR/$FILE" | cut -d' ' -f1)
SIZE=$(stat -c %s "$OUTDIR/$FILE")

INDEX="$OUTDIR/index.txt"
if [ ! -f "$INDEX" ]; then
    printf '# rozpkg index — name version sha256 size filename description\n' > "$INDEX"
fi
grep -v "^$NAME " "$INDEX" > "$INDEX.tmp" || true
printf '%s %s %s %s %s %s\n' "$NAME" "$VERSION" "$SHA" "$SIZE" "$FILE" "$DESC" >> "$INDEX.tmp"
mv "$INDEX.tmp" "$INDEX"

echo "  PKG     $OUTDIR/$FILE ($SIZE bytes)"
echo "          sha256 $SHA"
echo "  INDEX   $INDEX"
