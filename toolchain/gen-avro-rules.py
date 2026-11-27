#!/usr/bin/env python3
"""rozOS — অভ্র phonetic-এর নিয়ম C টেবিলে (ধাপ ২৩গ)

উৎস: github.com/mugli/libavrophonetic (MIT, (c) 2021 Mehdi Hasan Khan),
rulebasedconv/rules_source.go — Avro Classic Phonetic-এর হুবহু নিয়ম।
ব্যবহার: gen-avro-rules.py rules_source.go > userland/lib/rozbn_rules.h
(ফলাফল ফাইল git-এ রাখা হয়, তাই সাধারণ বিল্ডে এটা চালাতে হয় না।)"""
import re, sys

src = open(sys.argv[1], encoding="utf-8").read()
src = src[src.index("var avroClassicPhoneticRules"):]
src = src[src.index("{") :]
src = re.sub(r"//[^\n]*", "", src)
# bn_character.go-র ধ্রুবক (NFC রূপ) আর "a" + "b" জোড়া
for k, v in (("bnRRA", "\u09dc"), ("bnRHA", "\u09dd"), ("bnYYA", "\u09df")):
    src = src.replace(k, '"' + v + '"')
while True:
    n = re.sub(r'"((?:[^"\\]|\\.)*)"\s*\+\s*"((?:[^"\\]|\\.)*)"', r'"\1\2"', src)
    if n == src: break
    src = n
toks = re.findall(r'"(?:[^"\\]|\\.)*"|`[^`]*`|\[\]\w+\{|[{}\[\],:]|\w+', src)
pos = 0
CONST = {"suffix": "suffix", "prefix": "prefix", "vowel": "vowel", "notVowel": "!vowel",
         "consonant": "consonant", "notConsonant": "!consonant", "punctuation": "punctuation",
         "notPunctuation": "!punctuation", "exactly": "exactly", "notExactly": "!exactly"}

def value():
    global pos
    t = toks[pos]; pos += 1
    if t.startswith("[]") or t == "{":
        # একটা তালিকা বা বস্তু: key: value জোড়া থাকলে বস্তু
        items, obj = [], {}
        while toks[pos] != "}":
            if toks[pos] == ",":
                pos += 1; continue
            if pos + 1 < len(toks) and toks[pos + 1] == ":":
                k = toks[pos]; pos += 2
                obj[k] = value()
            else:
                items.append(value())
        pos += 1
        return obj if obj else items
    if t.startswith('"'):
        return bytes(t[1:-1], "utf-8").decode("unicode_escape").encode("latin1").decode("utf-8")
    if t.startswith("`"):
        return t[1:-1]
    return CONST.get(t, t)

rules = value()

def cstr(s):
    return '"' + "".join(c if c not in '"\\' else "\\" + c for c in s) + '"'

out = ["/* রুপান্তর: toolchain/gen-avro-rules.py — অভ্র Classic Phonetic-এর নিয়ম।",
       " * উৎস github.com/mugli/libavrophonetic (MIT, (c) 2021 Mehdi Hasan Khan)। হাতে বদলাবেন না। */",
       "#define AV_SUFFIX 1", "#define AV_PREFIX 0",
       "enum { AV_PUNCT = 0, AV_VOWEL, AV_CONSONANT, AV_EXACT };", ""]
conds, excs, rl = [], [], []
for r in rules:
    e0 = len(excs)
    for ex in r.get("exceptions", []):
        c0 = len(conds)
        for c in ex["ifAllMatch"]:
            k = c["is"]; neg = k.startswith("!"); k = k.lstrip("!")
            kind = {"punctuation": "AV_PUNCT", "vowel": "AV_VOWEL", "consonant": "AV_CONSONANT", "exactly": "AV_EXACT"}[k]
            conds.append("    { %s, %s, %d, %s }" % ("AV_SUFFIX" if c["when"] == "suffix" else "AV_PREFIX", kind,
                                                      1 if neg else 0, cstr(c.get("value", ""))))
        excs.append("    { %d, %d, %s }" % (c0, len(conds) - c0, cstr(ex["thenReplace"])))
    rl.append("    { %s, %s, %d, %d }" % (cstr(r["match"]), cstr(r.get("replace", "")), e0, len(excs) - e0))
out.append("static const struct av_cond av_conds[] = {\n" + ",\n".join(conds) + "\n};")
out.append("static const struct av_exc av_excs[] = {\n" + ",\n".join(excs) + "\n};")
out.append("static const struct av_rule av_rules[] = {\n" + ",\n".join(rl) + "\n};")
out.append("#define AV_NRULES %d" % len(rl))
print("\n".join(out))
sys.stderr.write("rules %d, exceptions %d, conditions %d\n" % (len(rl), len(excs), len(conds)))
