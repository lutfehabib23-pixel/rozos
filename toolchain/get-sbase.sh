#!/usr/bin/env bash
# rozOS — sbase (suckless Unix টুল) সোর্স আনা (ধাপ ১৯ছ)
#
# sbase (MIT লাইসেন্স — LICENSE ফাইল) থেকে grep, sed, sort, head, tail, find, xargs, cut, tr,
# diff-ধাঁচের cmp/comm, sha256sum ইত্যাদি → rozOS-এর /bin। যেগুলো rozOS-এ নিজেরই আছে
# (ls, cat, cp, mv, rm ...) সেগুলো বাদ (Makefile-এর SBASE_SKIP)।
# নির্দিষ্ট commit যাচাই করা হয়। প্রথমে suckless-এর নিজের রিপো, না পেলে GitHub-এর হুবহু কপি।
# ফল: ports/sbase/src/ (git-এ রাখা হয় না — .gitignore)।
set -euo pipefail

SBASE_COMMIT="${SBASE_COMMIT:-b30fb56804bfed69b45ef0e944d2e029e4d26258}"   # 2024-03-22
URLS="https://git.suckless.org/sbase https://github.com/michaelforney/sbase.git"

cd "$(dirname "$0")/.."
DEST=ports/sbase/src

if [ -f "$DEST/.rozos-commit" ] && [ "$(cat "$DEST/.rozos-commit")" = "$SBASE_COMMIT" ]; then
    echo "sbase আগেই আছে — কিছু করার নেই"
    exit 0
fi

ok=
for URL in $URLS; do
    rm -rf "$DEST"
    echo "==> $URL থেকে sbase আনা হচ্ছে"
    if git clone -q "$URL" "$DEST" 2>/dev/null &&
       git -C "$DEST" -c advice.detachedHead=false checkout -q "$SBASE_COMMIT" 2>/dev/null; then
        ok=1
        break
    fi
    echo "    পাওয়া গেল না — পরেরটা দেখা হচ্ছে"
done
if [ -z "$ok" ]; then
    rm -rf "$DEST"
    echo "sbase আনা গেল না (commit $SBASE_COMMIT) — ইন্টারনেট সংযোগ দেখুন"
    exit 1
fi
GOT=$(git -C "$DEST" rev-parse HEAD)
if [ "$GOT" != "$SBASE_COMMIT" ]; then
    echo "sbase commit মেলেনি: পাওয়া $GOT, আশা $SBASE_COMMIT — থামানো হলো"
    rm -rf "$DEST"
    exit 1
fi
rm -rf "$DEST/.git"
echo "$SBASE_COMMIT" > "$DEST/.rozos-commit"
echo "==> sbase তৈরি: $DEST"
