#!/usr/bin/env bash
# rozOS — ডিস্ক বড় করা, ডেটা অক্ষত রেখে (ধাপ ১২ক)
#
#   toolchain/disk-grow.sh IMG SIZE_MIB
#
# IMG এখন SIZE_MIB-এর চেয়ে ছোট হলে: image বড় করে, GPT-র backup header শেষে সরায়,
# partition 2 ("rozOS root", সবার শেষে) একই শুরুর sector থেকে ডিস্কের শেষ পর্যন্ত টানে,
# তারপর ext2 (e2fsck + resize2fs) বড় করে। boot partition (sda1) আর BIOS boot (sda3) বদলায় না।
# আগে IMG-এর একটা কপি IMG.before-grow-এ রাখে; সফল হলে সেটা মুছে দেয়।
# ছোট করা হয় না (ডেটা হারাতে পারে)।
set -euo pipefail
IMG=$1
SIZE=$2
for t in sgdisk e2fsck resize2fs; do
    command -v $t >/dev/null || { echo "$t নেই — sudo apt install gdisk e2fsprogs"; exit 1; }
done

CUR=$(( $(stat -c %s "$IMG") / 1048576 ))
if [ "$CUR" -ge "$SIZE" ]; then
    exit 0
fi
sgdisk -i 2 "$IMG" 2>/dev/null | grep -q "rozOS root" || { echo "  GROW    $IMG: partition 2 'rozOS root' নেই — বাদ"; exit 0; }

echo "  GROW    $IMG: $CUR MiB → $SIZE MiB (ফাইল অক্ষত; কপি: $IMG.before-grow)"
cp "$IMG" "$IMG.before-grow"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

info() { sgdisk -i "$1" "$IMG" | awk -v k="$2" '$0 ~ k {print $3}'; }
P2S=$(info 2 "First sector")
P2E=$(info 2 "Last sector")

# পুরোনো ext2-টা আলাদা করে রাখা
dd if="$IMG" of="$TMP/root.img" bs=512 skip="$P2S" count=$((P2E - P2S + 1)) status=none

truncate -s "${SIZE}M" "$IMG"
sgdisk -e "$IMG" >/dev/null
sgdisk -d 2 -n 2:"$P2S":0 -t 2:8300 -c 2:"rozOS root" "$IMG" >/dev/null
NP2E=$(info 2 "Last sector")
NP2N=$((NP2E - P2S + 1))

truncate -s $((NP2N * 512)) "$TMP/root.img"
e2fsck -fy "$TMP/root.img" >/dev/null 2>&1 || true
resize2fs -f "$TMP/root.img" >/dev/null 2>&1
e2fsck -fn "$TMP/root.img" >/dev/null 2>&1 || { echo "  GROW    ext2 পরীক্ষা ব্যর্থ — পুরোনোটা ফেরত"; mv "$IMG.before-grow" "$IMG"; exit 1; }
dd if="$TMP/root.img" of="$IMG" bs=512 seek="$P2S" conv=notrunc status=none
rm -f "$IMG.before-grow"
echo "  GROW    done: root partition now $((NP2N / 2048)) MiB"
