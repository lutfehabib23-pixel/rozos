#!/usr/bin/env bash
# rozOS — Lua 5.4.7 সোর্স আনা (ধাপ ১৬খ-১)
#
# Lua (MIT লাইসেন্স — লেখা আছে lua.h-এর শেষে) — rozOS-এর /bin/lua, /usr/lib/liblua.a।
# নির্দিষ্ট ট্যাগ + commit যাচাই করা হয়, যাতে সবার বিল্ড একই হয়। ফল: ports/lua/src/
# (git-এ রাখা হয় না — .gitignore)।
set -euo pipefail

LUA_TAG="${LUA_TAG:-v5.4.7}"
LUA_COMMIT="${LUA_COMMIT:-1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3}"
URL="https://github.com/lua/lua.git"

cd "$(dirname "$0")/.."
DEST=ports/lua/src

if [ -f "$DEST/.rozos-commit" ] && [ "$(cat "$DEST/.rozos-commit")" = "$LUA_COMMIT" ]; then
    echo "lua আগেই আছে ($LUA_TAG) — কিছু করার নেই"
    exit 0
fi

rm -rf "$DEST"
echo "==> $URL থেকে lua $LUA_TAG আনা হচ্ছে"
git -c advice.detachedHead=false clone -q --depth 1 --branch "$LUA_TAG" "$URL" "$DEST" \
    || { echo "lua আনা গেল না — ইন্টারনেট সংযোগ দেখুন"; exit 1; }
GOT=$(git -C "$DEST" rev-parse HEAD)
if [ "$GOT" != "$LUA_COMMIT" ]; then
    echo "lua commit মেলেনি: পাওয়া $GOT, আশা $LUA_COMMIT — থামানো হলো"
    rm -rf "$DEST"
    exit 1
fi
rm -rf "$DEST/.git"
echo "$LUA_COMMIT" > "$DEST/.rozos-commit"
echo "==> lua তৈরি: $DEST ($LUA_TAG)"
