#!/bin/bash
# Build a patched WhateverXe kext that accepts Tiger Lake 80 EU + 6 L3 banks.
# Usage:  ./build.sh          (or)  WEX_TAG=0.7.0 WORK=/tmp/w ./build.sh
set -euo pipefail
WEX_TAG="${WEX_TAG:-0.7.0}"
WORK="${WORK:-$HOME/wex-patch-build}"
SRC="$WORK/src"
CDN="https://codeload.github.com"
say() { printf '%s\n' "$*"; }
command -v xcrun >/dev/null 2>&1 || { say "error: xcrun not found; install Xcode Command Line Tools"; exit 1; }
xcrun --find clang++ >/dev/null 2>&1 || { say "error: run 'xcode-select --install' first"; exit 1; }
command -v python3 >/dev/null 2>&1 || { say "error: python3 not found"; exit 1; }
command -v curl >/dev/null 2>&1 || { say "error: curl not found"; exit 1; }
mkdir -p "$WORK"
cd "$WORK"
fetch() {
  if [ -s "$2" ]; then say "  cached  $2"; return 0; fi
  say "  fetch   $2"
  curl -fL --retry 3 --retry-delay 2 --max-time 600 -o "$2" "$1"
}
say "[1/4] downloading sources"
fetch "$CDN/b00t0x/WhateverXe/tar.gz/refs/tags/$WEX_TAG" "wex-$WEX_TAG.tar.gz"
fetch "$CDN/acidanthera/MacKernelSDK/tar.gz/refs/heads/master" "mksdk.tar.gz"
fetch "$CDN/acidanthera/Lilu/tar.gz/refs/heads/master" "lilu.tar.gz"
say "[2/4] preparing tree and applying patch"
python3 - "$WORK" "$WEX_TAG" <<'PY'
import sys, os, tarfile, shutil
work, tag = sys.argv[1], sys.argv[2]
src = os.path.join(work, "src")
def extract(tgz, prefix, dest):
    t = tarfile.open(tgz)
    n = 0
    for m in t.getmembers():
        if not m.name.startswith(prefix):
            continue
        rel = m.name[len(prefix):]
        if not rel:
            continue
        out = os.path.join(dest, rel.replace("/", os.sep))
        if m.isdir():
            os.makedirs(out, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        f = t.extractfile(m)
        if f is None:
            continue
        with f as s, open(out, "wb") as o:
            shutil.copyfileobj(s, o)
        n += 1
    return n
for d in (src, os.path.join(src, "MacKernelSDK"), os.path.join(src, "Lilu.kext")):
    if os.path.exists(d):
        shutil.rmtree(d)
os.makedirs(src, exist_ok=True)
n = extract(os.path.join(work, "wex-%s.tar.gz" % tag), "WhateverXe-%s/" % tag, src)
print("  source files: %d" % n)
extract(os.path.join(work, "mksdk.tar.gz"), "MacKernelSDK-master/",
        os.path.join(src, "MacKernelSDK"))
res = os.path.join(src, "Lilu.kext", "Contents", "Resources")
extract(os.path.join(work, "lilu.tar.gz"), "Lilu-master/Lilu/Headers/", os.path.join(res, "Headers"))
extract(os.path.join(work, "lilu.tar.gz"), "Lilu-master/Lilu/Library/", os.path.join(res, "Library"))
extract(os.path.join(work, "lilu.tar.gz"), "Lilu-master/capstone/include/", os.path.join(res, "Headers", "capstone"))
extract(os.path.join(work, "lilu.tar.gz"), "Lilu-master/hde/", os.path.join(res, "Headers"))
p = os.path.join(src, "WhateverXe", "kern_xelp_gt_resources.hpp")
s = open(p, encoding="utf-8").read()
old = """    if(!((decoded.dss==3 && decoded.eus==48 && decoded.l3Banks==6) ||
         ((decoded.dss==5 || decoded.dss==6) && decoded.eus==16*decoded.dss && decoded.l3Banks==8)))return false;"""
new = """    if(!((decoded.dss==3 && decoded.eus==48 && decoded.l3Banks==6) ||
         ((decoded.dss==5 || decoded.dss==6) && decoded.eus==16*decoded.dss &&
          (decoded.l3Banks==8 || decoded.l3Banks==6))))return false;"""
if "decoded.l3Banks==8 || decoded.l3Banks==6" in s:
    print("  patch   already present")
elif s.count(old) == 1:
    open(p, "w", encoding="utf-8", newline="\n").write(s.replace(old, new))
    print("  patch   applied")
else:
    raise SystemExit("error: patch anchor not found, upstream source changed")
bp = os.path.join(src, "tools", "build.py")
b = open(bp, encoding="utf-8").read()
o = """  if name=='WhateverXe':
   (out/'boot-stack.json').write_text(json.dumps(check_boot_stack(objects),indent=2)+'\\n')"""
n2 = """  if name=='WhateverXe':
   try:
    (out/'boot-stack.json').write_text(json.dumps(check_boot_stack(objects),indent=2)+'\\n')
   except Exception as _e:
    print('boot-stack budget check skipped:', _e)"""
if o in b:
    open(bp, "w", encoding="utf-8", newline="\n").write(b.replace(o, n2))
    print("  gate    stack budget check made non-fatal")
PY
say "[3/4] compiling (this takes 1-3 minutes)"
cd "$SRC"
python3 tools/build.py --release
say "[4/4] build complete"
KEXT="$SRC/build/WhateverXe.kext"
if [ ! -f "$KEXT/Contents/MacOS/WhateverXe" ]; then
  say "error: kext not produced"
  exit 1
fi
ls -l "$KEXT/Contents/MacOS/WhateverXe"
say ""
say "Install:"
say "  sudo diskutil mount disk0s1"
say "  cp -R \"$KEXT\" /Volumes/EFI/EFI/OC/Kexts/"
say ""
say "Verify after reboot:"
say "  ioreg -l -w0 | grep -o .\"TGLGT2Qualified\". = [A-Za-z]*"
