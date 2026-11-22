#!/usr/bin/env python3
# rozOS — ধাপ ১৯ক: C সহায়িকা (docs/cref/*.ref) যাচাই
#
#   ১. ফরম্যাট: প্রতিটা এন্ট্রিতে নাম, হেডার, সারাংশ; উদাহরণ থাকলে @end আছে; নাম দুবার নেই;
#      "@see"-এর প্রতিটা নামের পাতা আছে।
#   ২. সব আছে কিনা: --cover-এর প্রতিটা হেডারের সব ফাংশন (preprocessor দিয়ে বের করা) কোনো
#      এন্ট্রির @entry বা @also-তে আছে।
#   ৩. উদাহরণ: প্রতিটা উদাহরণ rozlibc দিয়ে কম্পাইল; "@check run" হলে চালিয়ে দেখা (rozlibc-এর
#      প্রোগ্রাম হোস্ট Linux-এও চলে — একই syscall), exit 0 আর ৫ সেকেন্ডের মধ্যে শেষ হতে হবে।
#
# Makefile থেকে: make cref-check  (ফেল করলে কোনটা কেন, ছাপে)
import argparse, os, re, shlex, subprocess, sys, tempfile


def parse(path):
    ents, e, multi, in_ex = [], None, None, False
    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\n").rstrip()
        if in_ex:
            if line == "@end":
                in_ex = False
                continue
            e["example"].append(raw.rstrip("\n"))
            continue
        if line.startswith("@"):
            kw, _, val = line[1:].partition(" ")
            val = val.strip()
            multi = None
            if kw == "entry":
                e = {"name": val, "also": "", "header": "", "syntax": [], "summary": "", "desc": [], "returns": [],
                     "see": "", "example": [], "check": "run", "input": [], "file": os.path.basename(path)}
                ents.append(e)
                continue
            if e is None:
                continue
            if kw in ("also", "header", "summary", "see", "check"):
                e[kw] = val
            elif kw == "syntax":
                e["syntax"].append(val)
            elif kw in ("desc", "returns", "input"):
                if val:
                    e[kw].append(val)
                multi = kw
            elif kw == "example":
                in_ex = True
            else:
                print(f"{path}: unknown field @{kw} in {e['name']}")
            continue
        if multi and e is not None:
            e[multi].append(raw.rstrip("\n"))
    if in_ex:
        print(f"{path}: example of {e['name']} has no @end")
    return ents


def own_decls(header, incdir):
    """হেডার নিজে যে ফাংশন ঘোষণা করে (যেগুলো সে #include করে সেগুলো বাদ)"""
    def decls(h):
        gccinc = subprocess.run(["gcc", "-print-file-name=include"], capture_output=True, text=True).stdout.strip()
        src = subprocess.run(["gcc", "-E", "-P", "-nostdinc", "-I" + incdir, "-isystem", gccinc, "-x", "c", "-"],
                             input=f"#include <{h}>\n", capture_output=True, text=True).stdout
        for _ in range(3):
            src = re.sub(r"\{[^{}]*\}", "", src)
        names = set()
        for stmt in src.split(";"):
            s = " ".join(stmt.split())
            if "typedef" in s:
                continue
            m = re.search(r"([A-Za-z_][A-Za-z_0-9]*)\s*\((?:[^()]|\([^()]*\))*\)\s*(__attribute__.*)?$", s)
            if m:
                names.add(m.group(1))
        return names
    own = decls(header)
    text = open(os.path.join(incdir, header)).read()
    for inc in re.findall(r'#\s*include\s*<([^>]+)>', text):
        if os.path.exists(os.path.join(incdir, inc)):
            own -= decls(inc)
    return {n for n in own if not n.startswith("_")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="docs/cref")
    ap.add_argument("--include", default="libc/include")
    ap.add_argument("--cover", default="")
    ap.add_argument("--cc", default="")
    ap.add_argument("--cflags", default="")
    ap.add_argument("--ldflags", default="")
    ap.add_argument("--libs", default="")
    ap.add_argument("--tcc", default="", help="tcc command too (examples must build with tcc in rozOS)")
    ap.add_argument("--only", default="", help="check only these entry names (comma list)")
    a = ap.parse_args()

    ents = []
    for f in sorted(os.listdir(a.dir)):
        if f.endswith(".ref"):
            ents += parse(os.path.join(a.dir, f))
    errors = 0
    names = {}
    for e in ents:
        for field in ("header", "summary"):
            if not e[field]:
                print(f"{e['file']}: {e['name']}: no @{field}")
                errors += 1
        for n in [e["name"]] + e["also"].split():
            if n in names and names[n] is not e:
                print(f"{e['file']}: '{n}' is in two entries ({names[n]['name']} and {e['name']})")
                errors += 1
            names[n] = e
    for e in ents:
        for s in e["see"].split():
            if s not in names:
                print(f"{e['file']}: {e['name']}: @see {s} has no page")
                errors += 1
    print(f"cref-check: {len(ents)} entries, {len(names)} names")

    # সব আছে কিনা
    for h in [x for x in a.cover.split() if x]:
        want = own_decls(h, a.include)
        missing = sorted(n for n in want if n not in names)
        print(f"cref-check: {h}: {len(want) - len(missing)}/{len(want)} functions documented")
        if missing:
            print("   missing: " + " ".join(missing))
            errors += len(missing)

    # উদাহরণ
    if a.cc:
        only = set(x for x in a.only.split(",") if x)
        tmp = tempfile.mkdtemp(prefix="cref-")
        built = ran = 0
        for i, e in enumerate(ents):
            if only and e["name"] not in only:
                continue
            if not e["example"] or e["check"] == "none":
                continue
            src = os.path.join(tmp, f"ex{i}.c")
            exe = os.path.join(tmp, f"ex{i}")
            open(src, "w").write("\n".join(e["example"]) + "\n")
            cmd = shlex.split(a.cc) + shlex.split(a.cflags) + ["-o", exe] + shlex.split(a.ldflags) + [src] + \
                shlex.split(a.libs)
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"{e['file']}: {e['name']}: example does not compile:\n{r.stderr[:1500]}")
                errors += 1
                continue
            if a.tcc:
                r = subprocess.run(shlex.split(a.tcc) + ["-c", src, "-o", exe + ".tcc.o"], capture_output=True, text=True)
                if r.returncode != 0:
                    print(f"{e['file']}: {e['name']}: example does not compile with tcc:\n{r.stderr[:1500]}")
                    errors += 1
                    continue
            built += 1
            if e["check"] != "run":
                continue
            wd = os.path.join(tmp, f"wd{i}")
            os.makedirs(wd, exist_ok=True)
            try:
                r = subprocess.run([exe], input="\n".join(e["input"]) + ("\n" if e["input"] else ""), cwd=wd,
                                   capture_output=True, text=True, timeout=5, env={"PATH": "/bin:/usr/bin", "HOME": wd,
                                                                                    "TZ": "UTC"})
            except subprocess.TimeoutExpired:
                print(f"{e['file']}: {e['name']}: example did not finish in 5 s")
                errors += 1
                continue
            ran += 1
            if r.returncode != 0:
                print(f"{e['file']}: {e['name']}: example exited with {r.returncode}\n{(r.stdout + r.stderr)[-800:]}")
                errors += 1
        print(f"cref-check: {built} examples compiled, {ran} run")
    if errors:
        print(f"cref-check: {errors} problem(s)")
        sys.exit(1)
    print("cref-check: all good")


if __name__ == "__main__":
    main()
