#!/usr/bin/env python3
# What the command line gegl does for `gegl IN -o OUT -- CHAIN...`, with
# GEGL's Python bindings, for when the gegl program (the gegl package on
# Debian and Ubuntu) is not installed: loads IN, applies the chain of
# operations and saves OUT. The chain is parsed by GEGL's
# gegl_create_chain, as in gegl, e.g.
#
#   tests/gegl-cli.py IN OUT wavelet:sharpen amount=1 radius=0.5
#
# (the words of the chain may not contain spaces)
import os
import sys

import gi
gi.require_version('Gegl', '0.4')
from gi.repository import Gegl

if len(sys.argv) < 4:
    sys.exit('usage: gegl-cli.py IN OUT OPERATION [KEY=VALUE...]')
src, dst = sys.argv[1:3]
chain = sys.argv[3:]
if any(any(c.isspace() for c in word) for word in chain):
    sys.exit('gegl-cli.py: the words of the chain may not contain spaces')
Gegl.init(None)
graph = Gegl.Node()
load = graph.create_child('gegl:load')
load.set_property('path', src)
start = graph.create_child('gegl:nop')
end = graph.create_child('gegl:nop')
save = graph.create_child('gegl:save')
save.set_property('path', dst)
load.link(start)
end.link(save)
# rel_dim: the size that relative values refer to, the image height as
# in gegl
height = load.get_bounding_box().height
Gegl.create_chain(' '.join(chain), start, end, 0.0, height,
                  os.path.dirname(os.path.abspath(src)))
save.process()
