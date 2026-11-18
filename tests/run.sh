#!/bin/sh
# Builds the operations into tests/output and checks them with the native
# GEGL that GIMP uses, without a window and without touching installed
# operations. Needs gimp-devtools next to this repo.
#
# 1. the command line gegl on a test image (tests/gegl-cli.py does the
#    same when the gegl program is not installed): denoise removes noise and keeps
#    the edge, sharpen steepens the edge and keeps flat areas;
# 2. tests/check.py: the results against the algorithms of the plug-ins,
#    tiled against whole rendering and the edge cases;
# 3. tests/check.py again, on a build with AddressSanitizer and
#    UndefinedBehaviorSanitizer.
#
# GEGL runs isolated from your folders (tests/isolate.sh, with
# gimp-devtools/gimp-run.sh): HOME and the XDG folders point into
# tests/output/gimp-home, so nothing lands in your own GEGL, babl or GIO
# folders. Before and after, it lists your folders of
# GIMP and the other apps (gimp-devtools/snapshot.sh) and fails if
# anything there changed.
#
# Prints PASS or FAIL per check and exits with 1 if any failed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out="$here/output"
build="$top/../gimp-devtools/gimp-build.sh"
mkdir -p "$out"
src=$top
GIMP_RUN_HOME=$out/gimp-home
export GIMP_RUN_HOME
# shellcheck source=SCRIPTDIR/isolate.sh
. "$here/isolate.sh"
snapshot_take "$out/snapshot-before.txt"

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
# GEGL's own operations, which GEGL_PATH replaces
gegl_ops=$(pkg-config --variable=pluginsdir gegl-0.4)
if command -v gegl >/dev/null 2>&1; then gegl_cli=gegl
else gegl_cli=tests/gegl-cli.py; fi
echo "== command line: $gegl_cli"

python3 "$here/make-image.py" "$out/test.png"
run () {
  name=$1; shift
  if [ "$gegl_cli" = gegl ]; then
    set -- gegl "$out/test.png" -o "$out/$name.png" -- "$@"
  else
    set -- python3 "$here/gegl-cli.py" "$out/test.png" "$out/$name.png" "$@"
  fi
  gimp_run --env=GEGL_PATH="$mod:$gegl_ops" -- "$@" 2>&1 |
    grep -v -i "leak\|warning\|^$" || true
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
snapshot_check "$out/snapshot-before.txt" "" || fails=1
if [ $fails = 0 ]; then echo "all passed"; else echo "FAILED"; fi
exit $fails
