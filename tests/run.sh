#!/bin/sh
# Runs wavelet:denoise and wavelet:sharpen from build/ on a test image with
# the GEGL of the Flatpak GIMP, and checks what they do: denoise removes
# noise and keeps the edge, sharpen steepens the edge and keeps flat areas.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(dirname "$here")
out="$here/output"
mkdir -p "$out"
python3 "$here/make-image.py" "$out/test.png"
# GEGL_PATH replaces GEGL's own list, and build/ holds files GEGL must not load
mod="$out/modules"; rm -rf "$mod"; mkdir -p "$mod"
cp "$top"/build/wavelet-*.so "$mod/"
run () {
  name=$1; shift
  flatpak run --filesystem="$here" --env=GEGL_PATH="$mod:/app/lib/gegl-0.4" --command=gegl org.gimp.GIMP \
    "$out/test.png" -o "$out/$name.png" -- "$@" 2>&1 | grep -v -i "leak\|warning\|^$" || true
}
run denoise wavelet:denoise threshold-1=2 threshold-2=2 threshold-3=2
run sharpen wavelet:sharpen amount=1 radius=0.5
python3 - "$out" <<'PY'
import sys
import numpy as np
from PIL import Image
out = sys.argv[1]
L = lambda n: np.asarray(Image.open(out + '/' + n).convert('L')).astype(float) / 255
src, den, sha = L('test.png'), L('denoise.png'), L('sharpen.png')
fails = 0
def check(ok, msg):
    global fails
    print(('ok   ' if ok else 'FAIL ') + msg)
    fails += not ok
n0, n1 = src[:, 20:130].std(), den[:, 20:130].std()
check(n1 < 0.6 * n0, 'denoise: noise %.4f -> %.4f' % (n0, n1))
e0, e1 = src[:, 205].mean() - src[:, 195].mean(), den[:, 205].mean() - den[:, 195].mean()
check(e1 > 0.9 * e0, 'denoise: edge contrast %.3f -> %.3f' % (e0, e1))
s0, s1 = np.abs(np.diff(src[100, 190:210])).max(), np.abs(np.diff(sha[100, 190:210])).max()
check(s1 > 1.1 * s0, 'sharpen: edge slope %.3f -> %.3f' % (s0, s1))
f0, f1 = src[:, 250:390].mean(), sha[:, 250:390].mean()
check(abs(f1 - f0) < 0.01, 'sharpen: flat area %.3f -> %.3f' % (f0, f1))
print('%d failed' % fails)
sys.exit(fails > 0)
PY
