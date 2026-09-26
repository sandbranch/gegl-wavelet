#!/bin/sh
# Builds the operations into tests/output and checks them with the GEGL of
# the Flatpak GIMP, without a window and without touching installed
# operations. Needs gimp-plugin-devtools next to this repo.
#
# 1. the command line gegl on a test image: denoise removes noise and keeps
#    the edge, sharpen steepens the edge and keeps flat areas;
# 2. tests/check.py: the results against the algorithms of the plug-ins,
#    tiled against whole rendering and the edge cases;
# 3. tests/check.py again, on a build with AddressSanitizer and
#    UndefinedBehaviorSanitizer.
#
# Prints PASS or FAIL per check and exits with 1 if any failed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out="$here/output"
build="$top/../gimp-plugin-devtools/gimp-build.sh"
mkdir -p "$out"

# GEGL_PATH replaces GEGL's own list, and a build folder holds files GEGL
# must not load, so the modules are copied into a folder of their own
modules () {
  if [ ! -d "$out/$1" ]; then
    "$build" "$top" meson setup "$out/$1" $2 > "$out/$1.log"
  fi
  "$build" "$top" ninja -C "$out/$1" >> "$out/$1.log"
  rm -rf "$out/$1-modules"; mkdir -p "$out/$1-modules"
  cp "$out/$1"/wavelet-*.so "$out/$1-modules/"
}
modules build
modules build-asan "-Db_sanitize=address,undefined -Db_lundef=false"
mod="$out/build-modules"

python3 "$here/make-image.py" "$out/test.png"
run () {
  name=$1; shift
  flatpak run --filesystem="$here" --env=GEGL_PATH="$mod:/app/lib/gegl-0.4" --command=gegl org.gimp.GIMP \
    "$out/test.png" -o "$out/$name.png" -- "$@" 2>&1 | grep -v -i "leak\|warning\|^$" || true
}
run denoise wavelet:denoise threshold-1=2 threshold-2=2 threshold-3=2
run sharpen wavelet:sharpen amount=1 radius=0.5
fails=0
python3 - "$out" <<'PY' || fails=1
import sys
import numpy as np
from PIL import Image
out = sys.argv[1]
L = lambda n: np.asarray(Image.open(out + '/' + n).convert('L')).astype(float) / 255
src, den, sha = L('test.png'), L('denoise.png'), L('sharpen.png')
fails = 0
def check(ok, msg):
    global fails
    print(('PASS ' if ok else 'FAIL ') + msg)
    fails += not ok
n0, n1 = src[:, 20:130].std(), den[:, 20:130].std()
check(n1 < 0.6 * n0, 'command line denoise: noise %.4f -> %.4f' % (n0, n1))
e0, e1 = src[:, 205].mean() - src[:, 195].mean(), den[:, 205].mean() - den[:, 195].mean()
check(e1 > 0.9 * e0, 'command line denoise: edge contrast %.3f -> %.3f' % (e0, e1))
s0, s1 = np.abs(np.diff(src[100, 190:210])).max(), np.abs(np.diff(sha[100, 190:210])).max()
check(s1 > 1.1 * s0, 'command line sharpen: edge slope %.3f -> %.3f' % (s0, s1))
f0, f1 = src[:, 250:390].mean(), sha[:, 250:390].mean()
check(abs(f1 - f0) < 0.01, 'command line sharpen: flat area %.3f -> %.3f' % (f0, f1))
print('%d of 4 checks failed' % fails)
sys.exit(fails > 0)
PY

echo "== checks"
python3 "$here/check.py" "$mod" || fails=1
echo "== checks with AddressSanitizer and UndefinedBehaviorSanitizer"
python3 "$here/check.py" "$out/build-asan-modules" --asan > "$out/asan.log" 2>&1 \
  || fails=1
grep -v '^PASS' "$out/asan.log"
if [ $fails = 0 ]; then echo "all passed"; else echo "FAILED"; fi
exit $fails
