#!/usr/bin/env bash
# rozOS — BearSSL 0.6 সোর্স আনা (ধাপ ১০: TLS / https)
#
# bearssl.org-এর মূল রিপোর আয়না নেই বলে Ubuntu/Debian-এর আর্কাইভ থেকে আসল 0.6
# (Debian-এর "+dfsg" প্যাকেজ: শুধু একটা Windows .exe সরানো, C কোড হুবহু) নেওয়া হয়,
# SHA-256 মিলিয়ে। ফল: ports/bearssl/src/ (git-এ রাখা হয় না — .gitignore)
set -euo pipefail

FILE="bearssl_0.6+dfsg.1.orig.tar.xz"
SHA256="102a625bd8f6155260867a50c508df8d8c3f796985dd744df1a68e5cd1a79004"
URLS=("http://archive.ubuntu.com/ubuntu/pool/universe/b/bearssl/$FILE"
      "http://mirrors.kernel.org/ubuntu/pool/universe/b/bearssl/$FILE"
      "https://deb.debian.org/debian/pool/main/b/bearssl/$FILE")

cd "$(dirname "$0")/.."
DEST=ports/bearssl/src

if [ -f "$DEST/.rozos-sha256" ] && [ "$(cat "$DEST/.rozos-sha256")" = "$SHA256" ]; then
    echo "BearSSL আগেই আছে — কিছু করার নেই"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for url in "${URLS[@]}"; do
    echo "==> $url"
    if curl -fsSL -o "$TMP/$FILE" "$url"; then
        break
    fi
done
[ -f "$TMP/$FILE" ] || { echo "BearSSL আনা গেল না — ইন্টারনেট সংযোগ দেখুন"; exit 1; }
echo "$SHA256  $TMP/$FILE" | sha256sum -c - >/dev/null || { echo "SHA-256 মেলেনি — ফাইল নষ্ট বা বদলানো"; exit 1; }

rm -rf "$DEST"
mkdir -p "$DEST"
tar -xJf "$TMP/$FILE" -C "$DEST" --strip-components=1
echo "$SHA256" > "$DEST/.rozos-sha256"
echo "==> BearSSL 0.6 তৈরি: $DEST"
