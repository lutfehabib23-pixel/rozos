#!/usr/bin/env bash
# rozOS — Limine বুটলোডারের binary রিলিজ নামানো ও host টুল `limine` বিল্ড
# ব্যবহার: ./toolchain/get-limine.sh   (প্রজেক্টের রুট ফোল্ডার থেকে)
set -euo pipefail

# Limine ২০২৬-এ আবার GitHub-এ (Limine-Bootloader) ফিরে গেছে; Codeberg এখন শুধু বিকল্প।
# ১২.x-এর কোনো binary ব্রাঞ্চ/ট্যাগ নেই; সর্বশেষ binary ব্রাঞ্চ হলো v11.x-binary।
# অন্য ব্রাঞ্চ নিতে: LIMINE_BRANCH=v10.x-binary ./toolchain/get-limine.sh
LIMINE_BRANCH="${LIMINE_BRANCH:-v11.x-binary}"
LIMINE_DIR="${LIMINE_DIR:-limine}"
PRIMARY_URL="https://github.com/Limine-Bootloader/Limine.git"
MIRROR_URL="https://codeberg.org/Limine/Limine.git"

log() { printf '\n\033[1;36m[rozOS-limine]\033[0m %s\n' "$*"; }

if [ -d "$LIMINE_DIR/.git" ]; then
    log "$LIMINE_DIR আগেই আছে — নতুন করে নামাতে চাইলে ফোল্ডারটি মুছে আবার চালান"
else
    log "Limine (${LIMINE_BRANCH}) নামানো হচ্ছে"
    git clone "$PRIMARY_URL" --branch="$LIMINE_BRANCH" --depth=1 "$LIMINE_DIR" \
    || { log "GitHub থেকে পাওয়া যায়নি, Codeberg চেষ্টা করছি";
         git clone "$MIRROR_URL" --branch="$LIMINE_BRANCH" --depth=1 "$LIMINE_DIR"; }
fi

log "host টুল 'limine' বিল্ড হচ্ছে"
make -C "$LIMINE_DIR"

# ---------- Limine প্রোটোকল হেডার (limine.h) ----------
# Limine 9.x থেকে limine.h আলাদা limine-protocol রিপোতে থাকে, binary রিলিজে আসে না।
PROTO_DIR="${PROTO_DIR:-limine-protocol}"
PROTO_PRIMARY="https://github.com/Limine-Bootloader/limine-protocol.git"
PROTO_MIRROR="https://codeberg.org/Limine/limine-protocol.git"
HEADER_DEST="kernel/boot/limine.h"

# আগের clone-এ limine.h না থাকলে (যেমন শুধু README আছে) সেটা মুছে নতুন করে নামানো হয়
if [ -d "$PROTO_DIR/.git" ] && \
   [ -z "$(find "$PROTO_DIR" -name 'limine.h' -not -path '*/.git/*' | head -n1)" ]; then
    log "$PROTO_DIR-এ limine.h নেই — মুছে নতুন করে নামানো হচ্ছে"
    rm -rf "$PROTO_DIR"
fi
if [ -d "$PROTO_DIR/.git" ]; then
    log "$PROTO_DIR আগেই আছে"
else
    log "limine-protocol নামানো হচ্ছে"
    git clone "$PROTO_PRIMARY" --depth=1 "$PROTO_DIR" \
    || { log "GitHub থেকে পাওয়া যায়নি, Codeberg চেষ্টা করছি";
         git clone "$PROTO_MIRROR" --depth=1 "$PROTO_DIR"; }
fi
# রিপোর ভেতরের ফোল্ডার কাঠামো বদলাতে পারে, তাই নির্দিষ্ট পাথ ধরে না নিয়ে limine.h খোঁজা হয়
HEADER_SRC="$(find "$PROTO_DIR" -name 'limine.h' -not -path '*/.git/*' | head -n1)"
if [ -z "$HEADER_SRC" ]; then
    echo "ত্রুটি: $PROTO_DIR-এর ভেতরে limine.h পাওয়া যায়নি। ফাইলগুলো:" >&2
    find "$PROTO_DIR" -maxdepth 2 -not -path '*/.git/*' >&2
    exit 1
fi
mkdir -p "$(dirname "$HEADER_DEST")"
cp "$HEADER_SRC" "$HEADER_DEST"
log "limine.h পাওয়া গেছে: $HEADER_SRC"

log "যাচাই:"
"./$LIMINE_DIR/limine" --version
echo "Limine commit: $(git -C "$LIMINE_DIR" rev-parse --short HEAD)"
ls -1 "$LIMINE_DIR" | grep -E 'limine-bios(-cd)?\.(sys|bin)|BOOTX64\.EFI|limine-uefi-cd\.bin' || true
echo "limine.h: $HEADER_DEST (commit $(git -C "$PROTO_DIR" rev-parse --short HEAD))"
grep -m1 -n 'LIMINE_BASE_REVISION' "$HEADER_DEST" || true
log "সম্পন্ন।"
