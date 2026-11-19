#!/usr/bin/env bash
# rozOS — Limine বুটলোডারের binary রিলিজ নামানো ও host টুল `limine` বিল্ড
# ব্যবহার: ./toolchain/get-limine.sh   (প্রজেক্টের রুট ফোল্ডার থেকে)
set -euo pipefail

LIMINE_VERSION="${LIMINE_VERSION:-10.5.0}"
LIMINE_DIR="${LIMINE_DIR:-limine}"
PRIMARY_URL="https://codeberg.org/Limine/Limine.git"
MIRROR_URL="https://github.com/Limine-Bootloader/Limine.git"

log() { printf '\n\033[1;36m[rozOS-limine]\033[0m %s\n' "$*"; }

if [ -d "$LIMINE_DIR/.git" ]; then
    log "$LIMINE_DIR আগেই আছে — নতুন করে নামাতে চাইলে ফোল্ডারটি মুছে আবার চালান"
else
    log "Limine v${LIMINE_VERSION} নামানো হচ্ছে"
    git clone "$PRIMARY_URL" --branch="v${LIMINE_VERSION}-binary" --depth=1 "$LIMINE_DIR" \
    || { log "Codeberg থেকে পাওয়া যায়নি, GitHub mirror চেষ্টা করছি";
         git clone "$MIRROR_URL" --branch="v${LIMINE_VERSION}-binary" --depth=1 "$LIMINE_DIR"; }
fi

log "host টুল 'limine' বিল্ড হচ্ছে"
make -C "$LIMINE_DIR"

log "যাচাই:"
"./$LIMINE_DIR/limine" --version
ls -1 "$LIMINE_DIR" | grep -E 'limine-bios(-cd)?\.(sys|bin)|BOOTX64\.EFI|limine-uefi-cd\.bin' || true
log "সম্পন্ন।"
