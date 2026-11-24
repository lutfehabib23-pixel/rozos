#!/usr/bin/env bash
# rozOS — TrueType ফন্ট আনা (ধাপ ২৩ক)
#
# সব ফন্ট খোলা লাইসেন্সের (SIL Open Font License 1.1) — rozOS-এর সাথে দেওয়া যায়।
# প্রতিটা ফাইলের sha256 মেলানো হয়, তাই প্রতিবার হুবহু একই ফন্ট আসে।
# ফল: ports/fonts/src/*.ttf আর LICENSE-*.txt (git-এ রাখা হয় না — .gitignore);
# Makefile এগুলো rozOS-এর /usr/share/fonts-এ বসায়।
#
#   UI        Inter 4.1 (Regular, Medium, SemiBold, Bold, Italic) — ডিফল্ট UI ফন্ট
#   mono      JetBrains Mono (Regular, Bold) — ডিফল্ট টার্মিনাল/Code ফন্ট; Fira Code
#   sans      Noto Sans, Roboto, Open Sans, Source Sans 3
#   serif     Noto Serif, Source Serif 4
#   বাংলা     Noto Sans Bengali, Noto Serif Bengali, Hind Siliguri, Tiro Bangla
#             (ধাপ ২৩খ-এ shaping-এর পরে ঠিকঠাক দেখা যাবে)
set -euo pipefail

cd "$(dirname "$0")/.."
DEST=ports/fonts/src
STAMP_VER=1                                   # তালিকা বদলালে বাড়ানো

INTER_URL="https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
INTER_SHA="9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e"
JB_COMMIT="19371302b95d218af43299bce79ddbddd0bc364d"
GF_COMMIT="06bfe0d62a98375cb09c942fa18e68bd32cff057"   # github.com/google/fonts
JB_RAW="https://raw.githubusercontent.com/JetBrains/JetBrainsMono/$JB_COMMIT"
GF_RAW="https://raw.githubusercontent.com/google/fonts/$GF_COMMIT"

# ফাইলের নাম  sha256  উৎস (URL)
LIST="
Inter-Regular.ttf 40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82 zip:extras/ttf/Inter-Regular.ttf
Inter-Medium.ttf 97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872 zip:extras/ttf/Inter-Medium.ttf
Inter-SemiBold.ttf 78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3 zip:extras/ttf/Inter-SemiBold.ttf
Inter-Bold.ttf 288316099b1e0a47a4716d159098005eef7c0066921f34e3200393dbdb01947f zip:extras/ttf/Inter-Bold.ttf
Inter-Italic.ttf bbc051dd204b5019a1aa0bc0ae2aa8a05ab13e7a3f979fa357631dc7feb6833a zip:extras/ttf/Inter-Italic.ttf
JetBrainsMono-Regular.ttf e6fd0d7e91550b3ed2b735d4312474362c4716edc4fc0577a0f61ed782d5aed1 $JB_RAW/fonts/ttf/JetBrainsMono-Regular.ttf
JetBrainsMono-Bold.ttf d22c4f3821d725eb01210d278d95dfcfcaadc34699a06658d47c8a5cc5830ada $JB_RAW/fonts/ttf/JetBrainsMono-Bold.ttf
FiraCode.ttf 9335b082b3c7850d98a64b584f3417f65355f3471278bb5eeb8c6c0e8657aeeb $GF_RAW/ofl/firacode/FiraCode%5Bwght%5D.ttf
NotoSans.ttf bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d $GF_RAW/ofl/notosans/NotoSans%5Bwdth,wght%5D.ttf
Roboto.ttf d7598e12c5dbef095ff8272cfc55da0250bd07fbdecbac8a530b9b277872a134 $GF_RAW/ofl/roboto/Roboto%5Bwdth,wght%5D.ttf
OpenSans.ttf 36643644f318a812aab2d2ed3bb98f8cf0872527f835fe9398d95fe6b9adb878 $GF_RAW/ofl/opensans/OpenSans%5Bwdth,wght%5D.ttf
SourceSans3.ttf 042fe2cc0b933e328410d7acbd0aa6a1873dca5aef81875f4bc214b08825c7b9 $GF_RAW/ofl/sourcesans3/SourceSans3%5Bwght%5D.ttf
NotoSerif.ttf 4d8e6761424656867019081a1a01336f3cb086982682698714054fc33f782713 $GF_RAW/ofl/notoserif/NotoSerif%5Bwdth,wght%5D.ttf
SourceSerif4.ttf 97b2d4da6e3cb494b5a1e66ae176914d852ccabef49e0c02c0df25f3e39aca0b $GF_RAW/ofl/sourceserif4/SourceSerif4%5Bopsz,wght%5D.ttf
NotoSansBengali.ttf dcd42978094e584a849c84a51450eeac40c8826057d566ea6d4b9627a403a05a $GF_RAW/ofl/notosansbengali/NotoSansBengali%5Bwdth,wght%5D.ttf
NotoSerifBengali.ttf 083180a7b47cf3b101ed55a8724aa1b38869daa9baf23b7cf198f9d225365aba $GF_RAW/ofl/notoserifbengali/NotoSerifBengali%5Bwdth,wght%5D.ttf
HindSiliguri-Regular.ttf 45071a7da902a74877f432722f525d00502d61aef71fca4cab3da8a23156c33d $GF_RAW/ofl/hindsiliguri/HindSiliguri-Regular.ttf
HindSiliguri-Bold.ttf d135765781d1a4aebc7dd4a2af221427ec8689060121d8a48b19bca3eeb3977e $GF_RAW/ofl/hindsiliguri/HindSiliguri-Bold.ttf
TiroBangla-Regular.ttf a763523dc3f90a71962cb8d9400b8f0a86b40b2cb01676f165101a2967127ae4 $GF_RAW/ofl/tirobangla/TiroBangla-Regular.ttf
"
# লাইসেন্স: ফাইলের নাম  উৎস
LICENSES="
LICENSE-Inter.txt zip:LICENSE.txt
LICENSE-JetBrainsMono.txt $JB_RAW/OFL.txt
LICENSE-FiraCode.txt $GF_RAW/ofl/firacode/OFL.txt
LICENSE-NotoSans.txt $GF_RAW/ofl/notosans/OFL.txt
LICENSE-Roboto.txt $GF_RAW/ofl/roboto/OFL.txt
LICENSE-OpenSans.txt $GF_RAW/ofl/opensans/OFL.txt
LICENSE-SourceSans3.txt $GF_RAW/ofl/sourcesans3/OFL.txt
LICENSE-NotoSerif.txt $GF_RAW/ofl/notoserif/OFL.txt
LICENSE-SourceSerif4.txt $GF_RAW/ofl/sourceserif4/OFL.txt
LICENSE-NotoSansBengali.txt $GF_RAW/ofl/notosansbengali/OFL.txt
LICENSE-NotoSerifBengali.txt $GF_RAW/ofl/notoserifbengali/OFL.txt
LICENSE-HindSiliguri.txt $GF_RAW/ofl/hindsiliguri/OFL.txt
LICENSE-TiroBangla.txt $GF_RAW/ofl/tirobangla/OFL.txt
"

if [ -f "$DEST/.rozos-fonts" ] && [ "$(cat "$DEST/.rozos-fonts")" = "$STAMP_VER" ]; then
    echo "ফন্ট আগেই আছে — কিছু করার নেই"
    exit 0
fi
command -v curl >/dev/null || { echo "curl নেই — sudo apt install curl"; exit 1; }
command -v unzip >/dev/null || { echo "unzip নেই — sudo apt install unzip"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$DEST"

echo "==> Inter 4.1 (zip, ~34 MB — একবারই)"
curl -sSfL -o "$TMP/inter.zip" "$INTER_URL"
echo "$INTER_SHA  $TMP/inter.zip" | sha256sum -c --quiet - || { echo "Inter zip-এর sha256 মেলেনি"; exit 1; }
unzip -q -o "$TMP/inter.zip" -d "$TMP/inter"

fetch() {                                     # fetch NAME SRC
    if [ "${2#zip:}" != "$2" ]; then
        cp "$TMP/inter/${2#zip:}" "$DEST/$1"
    else
        curl -sSfL -o "$DEST/$1" "$2"
    fi
}

echo "$LIST" | while read -r name sha src; do
    [ -n "$name" ] || continue
    echo "  $name"
    fetch "$name" "$(eval echo "$src")"
    case "$sha" in *-skip) ;; *)
        echo "$sha  $DEST/$name" | sha256sum -c --quiet - || { echo "$name: sha256 মেলেনি"; exit 1; } ;;
    esac
done
echo "$LICENSES" | while read -r name src; do
    [ -n "$name" ] || continue
    fetch "$name" "$(eval echo "$src")"
done
echo "$STAMP_VER" > "$DEST/.rozos-fonts"
echo "ফন্ট তৈরি: $(ls "$DEST"/*.ttf | wc -l)টা ফাইল ($(du -sh "$DEST" | cut -f1)) → $DEST"
