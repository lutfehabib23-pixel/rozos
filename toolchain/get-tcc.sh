#!/usr/bin/env bash
# rozOS — TinyCC (TCC) সোর্স আনা (ধাপ ৮)
#
# TCC-র মূল রিপো repo.or.cz/tinycc.git ("mob" ব্রাঞ্চ); GitHub-এর TinyCC/tinycc তার আয়না।
# নির্দিষ্ট একটা commit নেওয়া হয়, যাতে সবার বিল্ড একই হয়; তারপর ports/tcc/rozos.patch।
# ফল: ports/tcc/src/ (git-এ রাখা হয় না — .gitignore)
set -euo pipefail

TCC_COMMIT="${TCC_COMMIT:-0f004a0883c640528d69a006706f867d56c809e5}"   # 2026-11-18, mob
URLS=("https://github.com/TinyCC/tinycc.git" "https://repo.or.cz/tinycc.git")

cd "$(dirname "$0")/.."
DEST=ports/tcc/src

if [ -f "$DEST/.rozos-commit" ] && [ "$(cat "$DEST/.rozos-commit")" = "$TCC_COMMIT" ]; then
    echo "TCC আগেই আছে ($TCC_COMMIT) — কিছু করার নেই"
    exit 0
fi

rm -rf "$DEST"
for url in "${URLS[@]}"; do
    echo "==> $url থেকে আনা হচ্ছে"
    if git clone -q "$url" "$DEST"; then
        break
    fi
    rm -rf "$DEST"
done
[ -d "$DEST" ] || { echo "TCC আনা গেল না — ইন্টারনেট সংযোগ দেখুন"; exit 1; }

git -C "$DEST" checkout -q "$TCC_COMMIT"
patch -d "$DEST" -p1 < ports/tcc/rozos.patch
echo "$TCC_COMMIT" > "$DEST/.rozos-commit"
echo "==> TCC তৈরি: $DEST (commit ${TCC_COMMIT:0:12}, rozOS প্যাচ বসানো)"
