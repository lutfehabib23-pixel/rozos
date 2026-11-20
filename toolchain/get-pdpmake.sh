#!/usr/bin/env bash
# rozOS — pdpmake (Ron Yorston-এর public-domain POSIX make) সোর্স আনা (ধাপ ১১)
#
# rozOS-এর ভেতরে kernel ও প্রোগ্রাম বিল্ডের জন্য /bin/make। নির্দিষ্ট ট্যাগ + commit
# যাচাই করা হয়, যাতে সবার বিল্ড একই হয়। ফল: ports/pdpmake/src/ (git-এ রাখা হয় না)।
set -euo pipefail

PDPMAKE_TAG="${PDPMAKE_TAG:-2.0.4}"
PDPMAKE_COMMIT="${PDPMAKE_COMMIT:-0faaf258c9b3eaa9f5e4f6842e2484cbea6f23f8}"
URL="https://github.com/rmyorston/pdpmake.git"

cd "$(dirname "$0")/.."
DEST=ports/pdpmake/src

if [ -f "$DEST/.rozos-commit" ] && [ "$(cat "$DEST/.rozos-commit")" = "$PDPMAKE_COMMIT" ]; then
    echo "pdpmake আগেই আছে ($PDPMAKE_TAG) — কিছু করার নেই"
    exit 0
fi

rm -rf "$DEST"
echo "==> $URL থেকে pdpmake $PDPMAKE_TAG আনা হচ্ছে"
git -c advice.detachedHead=false clone -q --depth 1 --branch "$PDPMAKE_TAG" "$URL" "$DEST" \
    || { echo "pdpmake আনা গেল না — ইন্টারনেট সংযোগ দেখুন"; exit 1; }
GOT=$(git -C "$DEST" rev-parse HEAD)
if [ "$GOT" != "$PDPMAKE_COMMIT" ]; then
    echo "pdpmake commit মেলেনি: পাওয়া $GOT, আশা $PDPMAKE_COMMIT — থামানো হলো"
    rm -rf "$DEST"
    exit 1
fi
echo "$PDPMAKE_COMMIT" > "$DEST/.rozos-commit"
echo "==> pdpmake তৈরি: $DEST ($PDPMAKE_TAG)"
