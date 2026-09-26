# Runs inside the Python of the Flatpak GIMP (tests/check.py starts it),
# which has GEGL but no numpy: renders the cases of a job file with GEGL
# and writes the results as raw floats.
#
# The job is JSON: {"config": {GEGL config properties}, "cases": [...]}.
# Each case has "input" (raw floats), "format", "rect" [x, y, w, h], "op",
# "props", "output", "out_format", and optionally "as_format" (convert the
# input to this format first) and "tile" [w, h] (render the result in
# pieces of this size instead of at once). With "source" (an operation
# name) instead of "input", that operation is the input, e.g. one without
# bounds. With "extent" [x, y, w, h] the input buffer is that large, and
# holds the input in "rect" only.
import json
import sys

import gi
gi.require_version('Gegl', '0.4')
from gi.repository import Gegl

Gegl.init(None)
job = json.load(open(sys.argv[1]))
config = Gegl.config()
for name, value in job.get('config', {}).items():
    config.set_property(name, value)

report = {}
for case in job['cases']:
    x, y, w, h = case['rect']
    rect = Gegl.Rectangle.new(x, y, w, h)
    graph = Gegl.Node()
    if 'source' in case:
        source = graph.create_child(case['source'])
    else:
        src = Gegl.Buffer.new(case['format'], *case.get('extent', case['rect']))
        src.set(rect, case['format'], open(case['input'], 'rb').read())
        source = graph.create_child('gegl:buffer-source')
    if 'as_format' in case:
        converted = Gegl.Buffer.new(case['as_format'], x, y, w, h)
        converted.set(rect, case['as_format'],
                      src.get(rect, 1.0, case['as_format'],
                              Gegl.AbyssPolicy.NONE))
        src = converted
    if 'source' not in case:
        source.set_property('buffer', src)
    op = graph.create_child(case['op'])
    for name, value in case['props'].items():
        op.set_property(name, value)
    source.link(op)

    bbox = op.get_bounding_box()
    dst = Gegl.Buffer.new(case['out_format'], x, y, w, h)
    tile = case.get('tile')
    if tile:
        tw, th = tile
        for ty in range(y, y + h, th):
            for tx in range(x, x + w, tw):
                piece = Gegl.Rectangle.new(tx, ty, min(tw, x + w - tx),
                                           min(th, y + h - ty))
                op.blit_buffer(dst, piece, 0, Gegl.AbyssPolicy.NONE)
    else:
        op.blit_buffer(dst, rect, 0, Gegl.AbyssPolicy.NONE)

    with open(case['output'], 'wb') as f:
        f.write(dst.get(rect, 1.0, case['out_format'], Gegl.AbyssPolicy.NONE))
    report[case['output']] = {
        'bbox': [bbox.x, bbox.y, bbox.width, bbox.height],
        'tile_width': dst.get_property('tile-width'),
    }

json.dump(report, open(sys.argv[2], 'w'))
