#!/usr/bin/env bash
# rozOS — zlib 1.3.1 সোর্স আনা (ধাপ ১৬খ-১)
#
# zlib (zlib লাইসেন্স) — rozOS-এর /usr/lib/libz.a, /bin/gzip, /bin/gunzip।
# নির্দিষ্ট ট্যাগ + commit যাচাই করা হয়, যাতে সবার বিল্ড একই হয়। ফল: ports/zlib/src/
# (git-এ রাখা হয় না — .gitignore)।
set -euo pipefail

ZLIB_TAG="${ZLIB_TAG:-v1.3.1}"
ZLIB_COMMIT="${ZLIB_COMMIT:-51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf}"
URL="https://github.com/madler/zlib.git"

cd "$(dirname "$0")/.."
DEST=ports/zlib/src

if [ -f "$DEST/.rozos-commit" ] && [ "$(cat "$DEST/.rozos-commit")" = "$ZLIB_COMMIT" ]; then
    echo "zlib আগেই আছে ($ZLIB_TAG) — কিছু করার নেই"
    exit 0
fi

rm -rf "$DEST"
echo "==> $URL থেকে zlib $ZLIB_TAG আনা হচ্ছে"
git -c advice.detachedHead=false clone -q --depth 1 --branch "$ZLIB_TAG" "$URL" "$DEST" \
    || { echo "zlib আনা গেল না — ইন্টারনেট সংযোগ দেখুন"; exit 1; }
GOT=$(git -C "$DEST" rev-parse HEAD)
if [ "$GOT" != "$ZLIB_COMMIT" ]; then
    echo "zlib commit মেলেনি: পাওয়া $GOT, আশা $ZLIB_COMMIT — থামানো হলো"
    rm -rf "$DEST"
    exit 1
fi
rm -rf "$DEST/.git"
echo "$ZLIB_COMMIT" > "$DEST/.rozos-commit"
echo "==> zlib তৈরি: $DEST ($ZLIB_TAG)"
