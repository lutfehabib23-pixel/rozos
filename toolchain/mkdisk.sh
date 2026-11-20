#!/usr/bin/env bash
# rozOS — partition-সহ ডিস্ক image বানানো (ধাপ ১১খ)
#
#   toolchain/mkdisk.sh OUT SIZE_MIB ESP_MIB STAGE_DIR ESP_DIR [OLD_EXT2_IMAGE]
#
#   GPT:  partition 1 = "rozOS boot"  EFI System, FAT32 (ESP_MIB)  ← ESP_DIR-এর সব ফাইল
#         partition 2 = "rozOS root"  Linux data, ext2 (বাকিটা)    ← STAGE_DIR (mke2fs -d)
#   OLD_EXT2_IMAGE দিলে (make disk-migrate) partition 2 = সেই ext2-এর হুবহু কপি, মাপ বাড়ানো।
#         partition 3 = "BIOS boot" (sector 34-2047, ~1 MiB) — GPT ডিস্কে Limine-এর BIOS অংশ এখানে
#   শেষে Limine-এর BIOS অংশ ডিস্কে বসানো (UEFI-র জন্য ESP-তে EFI/BOOT/BOOTX64.EFI)।
#
# লাগে: sgdisk (gdisk), mkfs.fat (dosfstools), mcopy (mtools), mke2fs/resize2fs (e2fsprogs)
set -euo pipefail

OUT=$1 SIZE=$2 ESP=$3 STAGE=$4 ESPDIR=$5 OLD=${6:-}
LIMINE=${LIMINE:-limine/limine}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

for t in sgdisk mkfs.fat mcopy mke2fs; do
    command -v $t >/dev/null || { echo "$t নেই — sudo apt install gdisk dosfstools mtools e2fsprogs"; exit 1; }
done

rm -f "$OUT.new"
truncate -s "${SIZE}M" "$OUT.new"
sgdisk -o \
    -n 1:2048:+"${ESP}"M -t 1:ef00 -c 1:"rozOS boot" \
    -n 2:0:0 -t 2:8300 -c 2:"rozOS root" \
    -a 1 -n 3:34:2047 -t 3:ef02 -c 3:"BIOS boot" "$OUT.new" >/dev/null

first() { sgdisk -i "$1" "$OUT.new" | awk '/First sector/{print $3}'; }
last()  { sgdisk -i "$1" "$OUT.new" | awk '/Last sector/{print $3}'; }
P1S=$(first 1); P1E=$(last 1); P2S=$(first 2); P2E=$(last 2)
P1N=$((P1E - P1S + 1)); P2N=$((P2E - P2S + 1))

# ---- partition 1: FAT32 ----
truncate -s $((P1N * 512)) "$TMP/esp.img"
mkfs.fat -F 32 -s 1 -n ROZOSBOOT "$TMP/esp.img" >/dev/null
mcopy -s -Q -i "$TMP/esp.img" "$ESPDIR"/* ::/
dd if="$TMP/esp.img" of="$OUT.new" bs=512 seek="$P1S" conv=notrunc status=none

# ---- partition 2: ext2 ----
if [ -n "$OLD" ]; then
    cp "$OLD" "$TMP/root.img"
    truncate -s $((P2N * 512)) "$TMP/root.img"
    e2fsck -fy "$TMP/root.img" >/dev/null 2>&1 || true
    resize2fs -f "$TMP/root.img" >/dev/null 2>&1
else
    mke2fs -q -F -t ext2 -b 4096 -I 256 -L rozos -E root_owner=0:0 -d "$STAGE" \
        "$TMP/root.img" $((P2N / 2))k >/dev/null
fi
dd if="$TMP/root.img" of="$OUT.new" bs=512 seek="$P2S" conv=notrunc status=none

# ---- Limine (BIOS): stage 2 partition 3-এ ----
"$LIMINE" bios-install "$OUT.new" 3 >/dev/null 2>&1 || { echo "limine bios-install ব্যর্থ"; exit 1; }
mv "$OUT.new" "$OUT"
echo "  DISK    $OUT: ${SIZE} MiB GPT — sda1 FAT32 boot $((P1N / 2048)) MiB, sda2 ext2 root $((P2N / 2048)) MiB"
