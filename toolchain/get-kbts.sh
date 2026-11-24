#!/usr/bin/env bash
# rozOS — kb_text_shape আনা (ধাপ ২৩খ: বাংলা যুক্তাক্ষর — OpenType shaping)
#
# kb_text_shape v2.32 (Jimmy Lefevre, zlib লাইসেন্স): একটা C হেডার; লেখা ভাগ করে আর ফন্টের
# GSUB/GPOS নিয়ম দিয়ে অক্ষর সাজায় (ি-কার আগে, রেফ ওপরে, যুক্তাক্ষর)। librozgfx-এর
# rozgfx_shape.c এটা ব্যবহার করে। নির্দিষ্ট commit আর sha256 যাচাই করা হয়।
# ফল: ports/kbts/src/kb_text_shape.h (git-এ রাখা হয় না — .gitignore)।
set -euo pipefail

KB_COMMIT="4eec43b4c753a42b64aa8d7fd55dbce16a70343c"
KB_SHA="de2619d986753a643c8243855592e91e30613816c3944e08f8104868d7e789fd"
URL="https://raw.githubusercontent.com/JimmyLefevre/kb/$KB_COMMIT/kb_text_shape.h"

cd "$(dirname "$0")/.."
DEST=ports/kbts/src

if [ -f "$DEST/kb_text_shape.h" ] && echo "$KB_SHA  $DEST/kb_text_shape.h" | sha256sum -c --quiet - 2>/dev/null; then
    echo "kb_text_shape আগেই আছে — কিছু করার নেই"
    exit 0
fi
command -v curl >/dev/null || { echo "curl নেই — sudo apt install curl"; exit 1; }
mkdir -p "$DEST"
echo "==> kb_text_shape ($KB_COMMIT) আনা হচ্ছে"
curl -sSfL -o "$DEST/kb_text_shape.h" "$URL"
if ! echo "$KB_SHA  $DEST/kb_text_shape.h" | sha256sum -c --quiet -; then
    echo "kb_text_shape.h-এর sha256 মেলেনি — থামানো হলো"
    rm -f "$DEST/kb_text_shape.h"
    exit 1
fi
echo "kb_text_shape তৈরি → $DEST"
