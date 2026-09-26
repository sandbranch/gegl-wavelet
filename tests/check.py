#!/usr/bin/env python3
# Checks wavelet:sharpen and wavelet:denoise case by case: against a numpy
# version of the plug-ins' algorithms, whole against tiled rendering,
# thread counts, tile sizes, image origins, tiny images, uniform images,
# alpha, grayscale and values outside [0,1], NaN and infinity.
#
#   tests/check.py <folder with the .so files> [--asan]
#
# --asan: the .so files are built with -Db_sanitize=address,undefined;
# runs them with the sanitizer libraries of the SDK preloaded.
#
# The operations run in the Python of the Flatpak GIMP (tests/gegl-run.py),
# the comparisons here with numpy. Prints PASS or FAIL per case and exits
# with 1 if any failed.
import json
import math
import os
import subprocess
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, 'output', 'check')
f32 = np.float32
os.makedirs(out, exist_ok=True)
for f in os.listdir(out):
    os.remove(os.path.join(out, f))


# the plug-ins' algorithms, with the same float and double arithmetic

def mirror(i, size):
    period = 2 * (size - 1)
    if period == 0:
        return np.zeros_like(i)
    i = np.abs(i) % period
    return np.where(i < size, i, period - i)


def hat(a, axis, sc):
    n = a.shape[axis]
    i = np.arange(n)
    t = f32(2) * a + np.take(a, mirror(i - sc, n), axis) \
        + np.take(a, mirror(i + sc, n), axis)
    return t * f32(0.25)


def lowpass(a, sc):
    return hat(hat(a, 1, sc), 0, sc)


def ref_wavelet_sharpen(img, amount, radius):
    f = [img.astype(f32), None, None]
    hpass = lpass = 0
    for lev in range(5):
        lpass = (lev & 1) + 1
        f[lpass] = lowpass(f[hpass], 1 << lev)
        amt = f32(amount * math.exp(-(lev - radius) * (lev - radius) / 1.5)
                  + 1)
        f[hpass] = (f[hpass] - f[lpass]) * amt
        if hpass:
            f[0] = f[0] + f[hpass]
        hpass = lpass
    return f[0] + f[lpass]


def ref_sharpen(rgba, amount, radius, luminance):
    """rgba: (h, w, 4) R'G'B'A float"""
    if amount <= 0:
        return rgba.copy()
    val = [(rgba[..., c].astype(np.float64) * 255.0).astype(f32)
           for c in range(3)]
    if luminance:
        r, g, b = [v.astype(np.float64) for v in val]
        val = [(0.2990 * r + 0.5870 * g + 0.1140 * b).astype(f32),
               (-0.1687 * r - 0.3313 * g + 0.5000 * b + 128.0).astype(f32),
               (0.5000 * r - 0.4187 * g - 0.0813 * b + 128.0).astype(f32)]
    fimg = [(v.astype(np.float64) / 255.0).astype(f32) for v in val]
    for c in range(1 if luminance else 3):
        fimg[c] = ref_wavelet_sharpen(fimg[c], amount, radius)
    val = [(v.astype(np.float64) * 255.0).astype(f32) for v in fimg]
    if luminance:
        y, cb, cr = [v.astype(np.float64) for v in val]
        val = [(y + 1.40200 * (cr - 128.0)).astype(f32),
               (y - 0.34414 * (cb - 128.0) - 0.71414 * (cr - 128.0)).astype(f32),
               (y + 1.77200 * (cb - 128.0)).astype(f32)]
    res = rgba.copy()
    for c in range(3):
        res[..., c] = (val[c].astype(np.float64) / 255.0).astype(f32)
    return res


def ref_wavelet_denoise(img, threshold, low):
    f = [img.astype(f32), None, None]
    threshold = f32(threshold)
    hpass = lpass = 0
    with np.errstate(invalid='ignore'):
        for lev in range(5):
            lpass = (lev & 1) + 1
            f[lpass] = lowpass(f[hpass], 1 << lev)
            f[hpass] = f[hpass] - f[lpass]
            thold = f32(5.0 / (1 << 6) * math.exp(-2.6 * math.sqrt(lev + 1))
                        * 0.8002 / math.exp(-2.6))
            lp, hp = f[lpass], f[hpass]
            band = np.select([lp > 0.8, lp > 0.6, lp > 0.4, lp > 0.2],
                             [4, 3, 2, 1], 0)
            small = (hp < thold) & (hp > -thold)
            sq = hp.astype(np.float64) ** 2
            stdev = np.array([math.sqrt(sq[small & (band == k)].sum()
                                        / ((small & (band == k)).sum() + 1))
                              for k in range(5)])
            th = (threshold * stdev[band]).astype(f32).astype(np.float64)
            step = th - th * low
            h64 = hp.astype(np.float64)
            hp = np.where(hp < -th, h64 + step,
                          np.where(hp > th, h64 - step, h64 * low)).astype(f32)
            f[hpass] = hp
            if hpass:
                f[0] = f[0] + f[hpass]
            hpass = lpass
    return f[0] + f[lpass]


def ref_to_model(p, model):
    r, g, b = [c.astype(np.float64) for c in p]
    if model == 'ycbcr':
        return [(0.2990 * r + 0.5870 * g + 0.1140 * b).astype(f32),
                (-0.1687 * r - 0.3313 * g + 0.5000 * b + 0.5).astype(f32),
                (0.5000 * r - 0.4187 * g - 0.0813 * b + 0.5).astype(f32)]
    if model == 'lab':
        r, g, b = [np.sign(c) * np.abs(c) ** 2.2 for c in (r, g, b)]
        x = (0.412424 * r + 0.357579 * g + 0.180464 * b) / 0.95047
        y = 0.212656 * r + 0.715158 * g + 0.0721856 * b
        z = (0.0193324 * r + 0.119193 * g + 0.950444 * b) / 1.08883
        e = 216 / 24389.0
        fx, fy, fz = [np.where(t > e, np.cbrt(t), (24389 * t / 27.0 + 16)
                               / 116.0) for t in (x, y, z)]
        l = np.maximum((116 * fy - 16) / 116.0, 0)
        return [l.astype(f32), (500 * (fx - fy) / 500.0 / 2.0 + 0.5).astype(f32),
                (200 * (fy - fz) / 200.0 / 2.2 + 0.5).astype(f32)]
    return list(p)


def ref_from_model(p, model):
    a0, a1, a2 = [c.astype(np.float64) for c in p]
    if model == 'ycbcr':
        return [(a0 + 1.40200 * (a2 - 0.5)).astype(f32),
                (a0 - 0.34414 * (a1 - 0.5) - 0.71414 * (a2 - 0.5)).astype(f32),
                (a0 + 1.77200 * (a1 - 0.5)).astype(f32)]
    if model == 'lab':
        l, a, b = a0 * 116, (a1 - 0.5) * 1000, (a2 - 0.5) * 440
        y = (l + 16) / 116
        z = y - b / 200.0
        x = a / 500.0 + y
        k = lambda t: np.where(t ** 3 > 216 / 24389.0, t ** 3,
                               (116 * t - 16) * 27 / 24389.0)
        x, z = k(x) * 0.95047, k(z) * 1.08883
        y = np.where(l > 216 / 27.0, y ** 3, (116 * y - 16) * 27 / 24389.0)
        r = 3.24071 * x - 1.53726 * y - 0.498571 * z
        g = -0.969258 * x + 1.87599 * y + 0.0415557 * z
        b = 0.0556352 * x - 0.203996 * y + 1.05707 * z
        return [np.where(c < 0, 0, np.abs(c) ** (1 / 2.2)).astype(f32)
                for c in (r, g, b)]
    return list(p)


MODELS = {'ycbcr': 0, 'lab': 1, 'rgb': 2}


def ref_denoise(img, props):
    """img: (h, w, 4) R'G'B'A or (h, w, 2) Y'A float"""
    ch = img.shape[2]
    model = props.get('color-model', 'ycbcr')
    alpha_t = props.get('threshold-alpha', 0.0) \
        if props.get('denoise-alpha', False) else 0.0
    t = [props.get('threshold-1', 0.0), props.get('threshold-2', 0.4),
         props.get('threshold-3', 0.4)]
    s = [props.get('softness-%d' % (c + 1), 0.0) for c in range(3)]
    sa = props.get('softness-alpha', 0.0)
    if ch == 2:
        t, s = [t[0], alpha_t], [s[0], sa]
    else:
        t, s = t + [alpha_t], s + [sa]
    if all(v <= 0 for v in t):
        return img.copy()
    p = [img[..., c].astype(f32) for c in range(ch)]
    if ch > 2:
        p[:3] = ref_to_model(p[:3], model)
    for c in range(ch):
        if t[c] > 0:
            p[c] = ref_wavelet_denoise(p[c], t[c], s[c])
    if ch > 2:
        p[:3] = ref_from_model(p[:3], model)
    p[-1] = np.clip(p[-1], 0, 1)
    return np.dstack(p).astype(f32)


# test images

rng = np.random.default_rng(7)


def photo(h, w, ch=4, noise=0.05):
    """smooth shapes and noise, alpha varying"""
    yy, xx = np.mgrid[0:h, 0:w] / max(h, w, 1)
    base = [0.5 + 0.3 * np.sin(6 * xx + 2 * c) * np.cos(5 * yy - c)
            + 0.2 * ((xx - 0.5) ** 2 + (yy - 0.4) ** 2 < 0.06)
            for c in range(ch)]
    img = np.dstack(base) + rng.normal(0, noise, (h, w, ch))
    img[..., -1] = 0.25 + 0.75 * xx
    return np.clip(img, 0, 1).astype(f32)


# the cases: renders, grouped by GEGL settings, and checks on them

GROUPS = {
    'default': {},
    'threads1': {'threads': 1},
    'tiles16': {'threads': 4, 'tile-width': 16, 'tile-height': 16},
    'lowmem': {},
}
# address space for the lowmem group, in kB: less than denoise needs for
# the planes of its 20000 x 20000 case (9.6 GB)
LOWMEM = 6000000
renders = {}


def render(name, img, op, props, fmt=None, origin=(0, 0), tile=None,
           group='default', as_format=None, out_format=None, source=None,
           extent=None):
    ch = img.shape[2]
    fmt = fmt or ("R'G'B'A float" if ch == 4 else "Y'A float")
    props = dict(props)
    if 'color-model' in props:
        props['color-model'] = MODELS[props['color-model']]
    path = os.path.join(out, name)
    img.astype(f32).tofile(path + '.in')
    case = {'input': path + '.in', 'format': fmt,
            'rect': [origin[0], origin[1], img.shape[1], img.shape[0]],
            'op': op, 'props': props, 'output': path + '.out',
            'out_format': out_format or fmt}
    if tile:
        case['tile'] = tile
    if as_format:
        case['as_format'] = as_format
    if extent:
        case['extent'] = extent
    if source:
        del case['input']
        case['source'] = source
    renders[name] = (group, case, img.shape[:2] + (len(
        (out_format or fmt).split()[0].replace("'", '')),))


checks = []


def check(name, fn):
    checks.append((name, fn))


def maxdiff(a, b):
    with np.errstate(invalid='ignore'):
        d = np.abs(a.astype(np.float64) - b.astype(np.float64))
    return float(np.nanmax(d)) if d.size else 0.0


def close(a, b, tol):
    same_nan = np.array_equal(np.isnan(a), np.isnan(b))
    d = maxdiff(a, b)
    return same_nan and d <= tol, 'max diff %.3g (tolerance %.3g)' % (d, tol)


SHARPEN = 'wavelet:sharpen'
DENOISE = 'wavelet:denoise'
img = photo(70, 90)
gray = photo(70, 90, 2)

# sharpen: the algorithm of the plug-in, in both modes
for lum in (True, False):
    for amount, radius in ((1.0, 0.5), (10.0, 0.0), (0.3, 2.0)):
        n = 'sharpen-%s-%g-%g' % ('lum' if lum else 'rgb', amount, radius)
        props = {'amount': amount, 'radius': radius, 'luminance': lum}
        render(n, img, SHARPEN, props)
        check(n + ' equals the plug-in algorithm',
              lambda r, n=n, p=props: close(
                  r[n], ref_sharpen(img, p['amount'], p['radius'],
                                    p['luminance']), 2e-6))
    n = 'sharpen-%s' % ('lum' if lum else 'rgb')
    props = {'amount': 2.0, 'radius': 0.7, 'luminance': lum}
    render(n, img, SHARPEN, props)
    render(n + '-tiled', img, SHARPEN, props, tile=(13, 9))
    render(n + '-tiled-big', img, SHARPEN, props, tile=(64, 40))
    render(n + '-threads1', img, SHARPEN, props, group='threads1')
    render(n + '-tiles16', img, SHARPEN, props, group='tiles16')
    render(n + '-origin', img, SHARPEN, props, origin=(-37, 101))
    for v in ('tiled', 'tiled-big', 'threads1', 'tiles16', 'origin'):
        check('%s: %s equals whole' % (n, v),
              lambda r, n=n, v=v: close(r[n + '-' + v], r[n], 0))
    check(n + ': alpha unchanged',
          lambda r, n=n: close(r[n][..., 3], img[..., 3], 0))

render('sharpen-again', img, SHARPEN, {'amount': 2.0, 'radius': 0.7})
check('sharpen: deterministic',
      lambda r: close(r['sharpen-again'], r['sharpen-lum'], 0))
render('sharpen-amount0', img, SHARPEN, {'amount': 0.0})
check('sharpen: amount 0 returns the input',
      lambda r: close(r['sharpen-amount0'], img, 0))
flat = np.full((40, 50, 4), 0.37, f32)
flat[..., 1] = 0.6
for lum in (True, False):
    n = 'sharpen-flat-%d' % lum
    render(n, flat, SHARPEN, {'amount': 10.0, 'luminance': lum})
    # the YCbCr constants of the plug-in are not exactly inverse, which
    # moves colours by up to 1.5e-5 (a step of a 16-bit image)
    check(n + ': uniform image unchanged',
          lambda r, n=n, lum=lum: close(r[n], flat, 2e-5 if lum else 0))

# sizes up to below the largest scale (16) and just above its reach
for h, w in ((1, 1), (1, 7), (7, 1), (2, 2), (3, 40), (20, 20), (33, 5),
             (31, 64)):
    small = photo(h, w)
    for lum in (True, False):
        n = 'sharpen-%dx%d-%d' % (w, h, lum)
        props = {'amount': 3.0, 'radius': 1.0, 'luminance': lum}
        render(n, small, SHARPEN, props)
        check(n + ' equals the plug-in algorithm',
              lambda r, n=n, s=small, p=props: close(
                  r[n], ref_sharpen(s, 3.0, 1.0, p['luminance']), 2e-6))
        render(n + '-tiled', small, SHARPEN, props, tile=(3, 2))
        check(n + ': tiled equals whole',
              lambda r, n=n: close(r[n + '-tiled'], r[n], 0))

# grayscale is sharpened as RGB with equal channels
render('sharpen-gray', gray, SHARPEN, {'amount': 2.0},
       out_format="R'G'B'A float")
g4 = np.dstack([gray[..., 0]] * 3 + [gray[..., 1]])
check('sharpen: grayscale equals the plug-in algorithm',
      lambda r: close(r['sharpen-gray'], ref_sharpen(g4, 2.0, 0.5, True),
                      2e-6))

# linear input is sharpened on non-linear values
render('sharpen-linear', img, SHARPEN, {'amount': 2.0}, fmt='RGBA float',
       out_format="R'G'B'A float")
render('sharpen-linear-nl', img, SHARPEN, {'amount': 2.0}, fmt='RGBA float',
       as_format="R'G'B'A float", out_format="R'G'B'A float")
check('sharpen: linear input is processed as R\'G\'B\'',
      lambda r: close(r['sharpen-linear'], r['sharpen-linear-nl'], 1e-6))

# values outside [0,1], NaN and infinity: finite values stay finite, and a
# bad pixel only changes its reach (31 pixels), like any other pixel
wild = photo(100, 100)
wild[10:20, 10:20, :3] *= 3
wild[80:90, 10:20, :3] -= 1.5
bad = wild.copy()
bad[50, 50, 0] = np.nan
bad[50, 52, 1] = np.inf
render('sharpen-wild', wild, SHARPEN, {'amount': 5.0, 'luminance': False})
check('sharpen: values outside [0,1] as the plug-in algorithm',
      lambda r: close(r['sharpen-wild'],
                      ref_sharpen(wild, 5.0, 0.5, False), 1e-5))
for lum in (True, False):
    n = 'sharpen-nan-%d' % lum
    render(n, bad, SHARPEN, {'amount': 5.0, 'luminance': lum})
    render(n + '-ref', wild, SHARPEN, {'amount': 5.0, 'luminance': lum})

    def nan_local(r, n=n):
        far = np.ones((100, 100), bool)
        far[50 - 31:52 + 32, 50 - 31:52 + 32] = False
        same = np.array_equal(r[n][far], r[n + '-ref'][far])
        return same, 'unchanged beyond the reach: %s' % same
    check(n + ': NaN and infinity stay local', nan_local)

# denoise: the algorithm of the plug-in, per colour model
dprops = {'threshold-1': 1.5, 'threshold-2': 2.0, 'threshold-3': 3.0,
          'softness-2': 0.2}
for model in MODELS:
    n = 'denoise-' + model
    props = dict(dprops, **{'color-model': model})
    render(n, img, DENOISE, props)
    check(n + ' equals the plug-in algorithm',
          lambda r, n=n, p=props: close(r[n], ref_denoise(img, p), 2e-4))
    render(n + '-tiled', img, DENOISE, props, tile=(13, 9))
    render(n + '-threads1', img, DENOISE, props, group='threads1')
    render(n + '-tiles16', img, DENOISE, props, group='tiles16')
    render(n + '-origin', img, DENOISE, props, origin=(-37, 101))
    for v in ('tiled', 'threads1', 'tiles16', 'origin'):
        check('%s: %s equals whole' % (n, v),
              lambda r, n=n, v=v: close(r[n + '-' + v], r[n], 0))
    check(n + ': alpha unchanged',
          lambda r, n=n: close(r[n][..., 3], img[..., 3], 0))

    # softness 1 keeps all detail: only the colour conversions remain
    soft = dict(props, **{'softness-%d' % c: 1.0 for c in (1, 2, 3)})
    render(n + '-soft1', img, DENOISE, soft)
    check(n + ': softness 1 keeps the image',
          lambda r, n=n: close(r[n + '-soft1'][..., :3], img[..., :3],
                               2e-3 if 'lab' in n else 2e-4))

    render(n + '-zero', img, DENOISE, {'color-model': model,
                                       'threshold-2': 0.0,
                                       'threshold-3': 0.0})
    check(n + ': zero thresholds return the input',
          lambda r, n=n: close(r[n + '-zero'], img, 0))

render('denoise-again', img, DENOISE, dprops)
check('denoise: deterministic',
      lambda r: close(r['denoise-again'], r['denoise-ycbcr'], 0))

aprops = dict(dprops, **{'denoise-alpha': True, 'threshold-alpha': 3.0})
noisy_alpha = img.copy()
noisy_alpha[..., 3] = np.clip(noisy_alpha[..., 3]
                              + rng.normal(0, 0.05, img.shape[:2]), 0, 1)
render('denoise-alpha', noisy_alpha, DENOISE, aprops)
check('denoise: alpha denoised as the plug-in algorithm',
      lambda r: close(r['denoise-alpha'], ref_denoise(noisy_alpha, aprops),
                      2e-4))
check('denoise: alpha denoised and in [0,1]',
      lambda r: (r['denoise-alpha'][..., 3].std()
                 < noisy_alpha[..., 3].std() and
                 r['denoise-alpha'][..., 3].min() >= 0 and
                 r['denoise-alpha'][..., 3].max() <= 1, ''))

for name, im, props in (('gray', gray, dprops), ('gray-alpha', gray, aprops)):
    n = 'denoise-' + name
    render(n, im, DENOISE, props)
    check(n + ' equals the plug-in algorithm',
          lambda r, n=n, im=im, p=props: close(r[n], ref_denoise(im, p),
                                               2e-4))
    render(n + '-tiled', im, DENOISE, props, tile=(13, 9))
    check(n + ': tiled equals whole',
          lambda r, n=n: close(r[n + '-tiled'], r[n], 0))
check('denoise-gray: alpha unchanged',
      lambda r: close(r['denoise-gray'][..., 1], gray[..., 1], 0))
render('denoise-gray-noalpha', gray[..., :1], DENOISE, dprops,
       fmt="Y' float")
check('denoise: gray without alpha equals the plug-in algorithm',
      lambda r: close(r['denoise-gray-noalpha'],
                      ref_denoise(np.dstack([gray[..., 0], np.ones_like(
                          gray[..., 0])]), dprops)[..., :1], 2e-4))

render('denoise-linear', img, DENOISE, dprops, fmt='RGBA float',
       out_format="R'G'B'A float")
render('denoise-linear-nl', img, DENOISE, dprops, fmt='RGBA float',
       as_format="R'G'B'A float", out_format="R'G'B'A float")
check('denoise: linear input is processed as R\'G\'B\'',
      lambda r: close(r['denoise-linear'], r['denoise-linear-nl'], 1e-6))

big = dict(dprops, **{'threshold-1': 10.0, 'threshold-2': 10.0,
                      'threshold-3': 10.0})
for h, w in ((1, 1), (1, 7), (7, 1), (2, 2), (3, 40), (20, 20), (33, 5)):
    small = photo(h, w)
    n = 'denoise-%dx%d' % (w, h)
    render(n, small, DENOISE, big)
    check(n + ' equals the plug-in algorithm',
          lambda r, n=n, s=small: close(r[n], ref_denoise(s, big), 2e-4))

for model in MODELS:
    n = 'denoise-flat-' + model
    render(n, flat, DENOISE, dict(big, **{'color-model': model}))
    check(n + ': uniform image unchanged',
          lambda r, n=n: close(r[n][..., :3], flat[..., :3], 2e-3))

# out of range, NaN and infinity: finite input gives finite output
for model in MODELS:
    n = 'denoise-wild-' + model
    props = dict(dprops, **{'color-model': model})
    render(n, wild, DENOISE, props)
    check(n + ': values outside [0,1] give finite results',
          lambda r, n=n: (bool(np.isfinite(r[n]).all()),
                          'non-finite: %d' % (~np.isfinite(r[n])).sum()))
    render(n + '-nan', bad, DENOISE, props)

    def nan_local(r, n=n):
        far = np.ones((100, 100), bool)
        far[50 - 31:52 + 32, 50 - 31:52 + 32] = False
        ok = bool(np.isfinite(r[n + '-nan'][far]).all())
        return ok, 'finite beyond the reach: %s' % ok
    check(n + ': NaN and infinity stay local', nan_local)

# an input without bounds, such as a pattern, is not read without end: the
# noise of denoise cannot be measured on it, so it is passed through
for op, props in ((SHARPEN, {'amount': 2.0}), (DENOISE, dprops)):
    n = op.split(':')[1] + '-infinite'
    render(n, np.zeros((30, 40, 4), f32), op, props, origin=(-5, -5),
           source='gegl:checkerboard')
    render(n + '-plain', np.zeros((30, 40, 4), f32), 'gegl:nop', {},
           origin=(-5, -5), source='gegl:checkerboard')
check('sharpen: input without bounds is sharpened',
      lambda r: (bool(np.isfinite(r['sharpen-infinite']).all()) and
                 maxdiff(r['sharpen-infinite'], r['sharpen-infinite-plain'])
                 > 0.01, ''))
check('denoise: input without bounds is passed through',
      lambda r: close(r['denoise-infinite'], r['denoise-infinite-plain'], 0))

# denoise leaves an image as it is when the memory for it is refused
render('denoise-lowmem', img[:4, :8], DENOISE, dprops, group='lowmem',
       extent=[0, 0, 20000, 20000])
check('denoise: image left as it is without memory',
      lambda r: close(r['denoise-lowmem'], img[:4, :8], 0))


# run and compare

def run(modules, asan):
    results = {}
    gegl_path = modules + ':/app/lib/gegl-0.4'
    for group, config in GROUPS.items():
        cases = [c for g, c, _ in renders.values() if g == group]
        job = os.path.join(out, 'job-%s.json' % group)
        report = os.path.join(out, 'report-%s.json' % group)
        json.dump({'config': config, 'cases': cases}, open(job, 'w'))
        cmd = ['flatpak', 'run', '--filesystem=' + here,
               '--filesystem=' + modules, '--env=GEGL_PATH=' + gegl_path]
        if asan:
            cmd += ['--devel',
                    '--env=LD_PRELOAD=libasan.so.8:libubsan.so.1',
                    '--env=ASAN_OPTIONS=detect_leaks=0:abort_on_error=0',
                    '--env=UBSAN_OPTIONS=print_stacktrace=1']
        run = ['python3', os.path.join(here, 'gegl-run.py'), job, report]
        if group == 'lowmem':
            # AddressSanitizer reserves more address space than that
            if asan:
                continue
            run = ['sh', '-c', 'ulimit -v %d && exec "$@"' % LOWMEM,
                   'sh'] + run
        cmd += ['--command=' + run[0], 'org.gimp.GIMP'] + run[1:]
        p = subprocess.run(cmd, capture_output=True, text=True)
        log = p.stdout + p.stderr
        if p.returncode != 0 or 'runtime error' in log or 'ERROR: Address' \
                in log:
            print(log)
            print('FAIL rendering the %s cases (exit %d)'
                  % (group, p.returncode))
            sys.exit(1)
        results.update(json.load(open(report)))
    return results


def main():
    args = sys.argv[1:]
    asan = '--asan' in args
    if asan:
        args.remove('--asan')
    modules = os.path.abspath(args[0])
    report = run(modules, asan)
    r = {}
    for name, (group, case, shape) in renders.items():
        if case['output'] not in report:
            continue
        r[name] = np.fromfile(case['output'], f32).reshape(shape)
        x, y, w, h = case['rect']
        if report[case['output']]['bbox'] != case.get('extent', [x, y, w, h]) \
                and 'source' not in case:
            print('FAIL %s: bounding box %s, not %s'
                  % (name, report[case['output']]['bbox'], case['rect']))
            sys.exit(1)
    tw = [v['tile_width'] for k, v in report.items()]
    fails = 0
    if 16 not in tw:
        print('FAIL the tile size setting had no effect')
        fails += 1
    for name, fn in checks:
        try:
            ok, msg = fn(r)
        except KeyError as e:
            print('SKIP %s: %s not rendered here' % (name, e))
            continue
        except Exception as e:
            ok, msg = False, 'error: %r' % e
        print('%s %s%s' % ('PASS' if ok else 'FAIL', name,
                           (': ' + msg) if msg else ''))
        fails += not ok
    print('%d of %d checks failed' % (fails, len(checks)))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
