#!/usr/bin/env bash
# rozOS — ডিস্ক image-এর partition অফসেট (ধাপ ১১খ): "ESP_BYTES ROOT_BYTES" প্রিন্ট করে;
# পুরোনো (partition ছাড়া, পুরোটা ext2) ডিস্ক হলে "old"।
set -euo pipefail
IMG=$1
if ! sgdisk -p "$IMG" 2>/dev/null | grep -q "rozOS root"; then
    echo old
    exit 0
fi
p() { sgdisk -i "$1" "$IMG" | awk '/First sector/{print $3}'; }
echo "$(( $(p 1) * 512 )) $(( $(p 2) * 512 ))"
