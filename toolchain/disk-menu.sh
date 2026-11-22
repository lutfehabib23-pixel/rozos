#!/usr/bin/env bash
# rozOS — PC থেকে ডিস্কের boot মেনু ঠিক করা (ধাপ ১২ক)
#
#   disk-menu.sh IMG ESP_BYTES [OLD_TAG]
#
# make disk-update নতুন factory kernel লেখার পর এটা ডাকে। শেষবার যে factory kernel-কে
# ডিফল্ট করা হয়েছিল তার "ROZOS-BUILD:..." tag থাকে boot partition-এর
# /kernels/factory-seen-এ (OLD_TAG এখন আর লাগে না, পুরোনো ডাকার জন্য রাখা)। নতুন
# factory-র tag আলাদা হলে (মানে PC-তে নতুন kernel বানানো হয়েছে) factory-কে ডিফল্ট করে
# /limine.conf আবার লেখে — ঠিক rozOS-এর kinstall-এর write_menu() যে রূপে লেখে সেভাবে,
# যাতে rozOS-এর ভেতরে বানানো পুরোনো kernel (যেমন 1.1) আর ডিফল্ট হয়ে না থাকে।
# একই factory থাকলে কিছুই বদলায় না (rozOS-এ kinstall -d দিয়ে বাছা ডিফল্ট রক্ষা পায়)।
# মাঝপথে থামলেও (Ctrl+C) পরের disk-update আবার চেষ্টা করে, কারণ factory-seen শেষে লেখা হয়।
# mtools কখনো প্রশ্ন করে অপেক্ষা না করে: সব কমান্ডের stdin /dev/null, আর ফোল্ডার
# থাকলে mmd ডাকা হয় না (থাকা নামে mmd প্রশ্ন করে আটকে থাকে)।
set -euo pipefail
exec </dev/null
IMG=$1
M="$IMG@@$2"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

tag_of() {  # FAT-এর ভেতরের ফাইল → tag (না পেলে খালি)
    # আগের কপি মুছে নেওয়া: থাকা লোকাল ফাইলে mcopy -o মানে না, /dev/tty-তে প্রশ্ন করে আটকে থাকে
    rm -f "$TMP/k.elf"
    if mcopy -o -i "$M" "::$1" "$TMP/k.elf" 2>/dev/null; then
        grep -a -o -m1 'ROZOS-BUILD:[ -~]*' "$TMP/k.elf" | head -n1 || true
    fi
}
field() { echo "$1" | cut -d: -f"$2"; }          # 2=id 3=cc 4=where
datef() { echo "$1" | cut -d: -f5-; }             # তারিখে ':' আছে

NEW_TAG=$(tag_of /rozos-factory.elf)
SEEN_TAG=""
if mcopy -o -i "$M" ::/kernels/factory-seen "$TMP/seen" 2>/dev/null; then
    SEEN_TAG=$(head -n1 "$TMP/seen" | tr -d '\r')
fi
if [ -z "$NEW_TAG" ] || [ "$NEW_TAG" = "$SEEN_TAG" ]; then
    exit 0
fi

# ইনস্টল করা build-গুলো (নতুনটা আগে)
IDS=()
if mcopy -o -i "$M" ::/kernels/list "$TMP/list" 2>/dev/null; then
    while IFS= read -r l; do
        l=${l%%[$'\r' ]*}
        [[ $l =~ ^[0-9.]+$ ]] && IDS+=("$l")
    done < "$TMP/list"
fi

# ধাপ ১৩: kinstall --resolution-এর মান (না থাকলে Limine নিজে বাছে)
RES=""
if mcopy -o -i "$M" ::/kernels/resolution "$TMP/res" 2>/dev/null; then
    RES=$(head -n1 "$TMP/res" | tr -d '\r ')
fi

entry() {  # title path extra
    printf '/%s\n    protocol: limine\n' "$1"
    [ -n "$RES" ] && printf '    resolution: %s\n' "$RES"
    printf '    path: boot():%s\n    cmdline: console=fb,serial%s\n' "$2" "$3"
    printf '    module_path: boot():/initrd.tar\n    module_cmdline: initrd\n\n'
}

{
    printf '# rozOS boot menu — kinstall লেখে (হাতে বদলালে পরের kinstall-এ নতুন করে লেখা হবে)\n'
    printf '# help kinstall, help boot-menu\n\n'
    printf 'timeout: 3\n'
    # ধাপ ১৯খ: বুট পর্দার ছবি (Makefile-এর disk-update এটা boot partition-এ রাখে)
    if mdir -i "$M" ::/boot-wallpaper.jpg >/dev/null 2>&1; then
        printf 'wallpaper: boot():/boot-wallpaper.jpg\nwallpaper_style: centered\nbackdrop: 000000\nterm_background: ff000000\n'
    fi
    printf 'default_entry: %d\n\n' $(( ${#IDS[@]} + 1 ))
    n=0
    for id in "${IDS[@]}"; do
        t=$(tag_of "/kernels/rozos-$id.elf")
        cc=$(field "$t" 3); [ -n "$cc" ] || cc="?"
        suffix=""; [ $n -eq 0 ] && suffix=" - newest"
        entry "rozOS build $id ($cc, $(datef "$t"))$suffix" "/kernels/rozos-$id.elf" ""
        n=$((n + 1))
    done
    entry "rozOS factory (build $(field "$NEW_TAG" 2), gcc) - default" /rozos-factory.elf ""
    entry "rozOS factory (text mode, no desktop)" /rozos-factory.elf " text"
    entry "rozOS factory (no init: kernel monitor)" /rozos-factory.elf " noinit"
} > "$TMP/limine.conf"

echo factory > "$TMP/default"
mdir -i "$M" ::/kernels >/dev/null 2>&1 || mmd -i "$M" ::/kernels
mcopy -o -i "$M" "$TMP/default" ::/kernels/default
mcopy -o -i "$M" "$TMP/limine.conf" ::/limine.conf
echo "$NEW_TAG" > "$TMP/seen"
mcopy -o -i "$M" "$TMP/seen" ::/kernels/factory-seen
echo "  MENU    new factory kernel (build $(field "$NEW_TAG" 2)) is now the default boot entry"
