#!/usr/bin/env bash
# rozOS — FreeType সোর্স আনা (ধাপ ২৩ক)
#
# FreeType 2.13.3 (FreeType License — BSD-ধাঁচের, শুধু কৃতিত্ব দিতে হয়; docs/FTL.TXT)।
# TrueType/OpenType ফন্টকে মসৃণ (anti-aliased) আর hinted অক্ষরে আঁকে — ছোট মাপেও ঝকঝকে।
# librozgfx-এর ভেতরে যায় (rozgfx_ft.c), তাই প্রোগ্রামে আলাদা -lfreetype লাগে না।
# নির্দিষ্ট commit যাচাই করা হয়। ফল: ports/freetype/src/ (git-এ রাখা হয় না — .gitignore)।
set -euo pipefail

FT_TAG="VER-2-13-3"
FT_COMMIT="${FT_COMMIT:-42608f77f20749dd6ddc9e0536788eaad70ea4b5}"
URLS="https://github.com/freetype/freetype https://gitlab.freedesktop.org/freetype/freetype.git"

cd "$(dirname "$0")/.."
DEST=ports/freetype/src

if [ -f "$DEST/.rozos-commit" ] && [ "$(cat "$DEST/.rozos-commit")" = "$FT_COMMIT" ]; then
    echo "FreeType আগেই আছে — কিছু করার নেই"
    exit 0
fi

ok=
for URL in $URLS; do
    rm -rf "$DEST"
    echo "==> $URL থেকে FreeType ($FT_TAG) আনা হচ্ছে"
    if git clone -q --depth 1 --branch "$FT_TAG" "$URL" "$DEST" 2>/dev/null; then
        ok=1
        break
    fi
    echo "    পাওয়া গেল না — পরেরটা দেখা হচ্ছে"
done
if [ -z "$ok" ]; then
    rm -rf "$DEST"
    echo "FreeType আনা গেল না — ইন্টারনেট সংযোগ দেখুন"
    exit 1
fi
GOT=$(git -C "$DEST" rev-parse HEAD)
if [ "$GOT" != "$FT_COMMIT" ]; then
    echo "FreeType commit মেলেনি: পাওয়া $GOT, আশা $FT_COMMIT — থামানো হলো"
    rm -rf "$DEST"
    exit 1
fi
rm -rf "$DEST/.git"
echo "$FT_COMMIT" > "$DEST/.rozos-commit"
echo "FreeType তৈরি → $DEST"
