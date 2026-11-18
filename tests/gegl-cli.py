#!/usr/bin/env python3
# What the command line gegl does for `gegl IN -o OUT -- OP KEY=VALUE...`,
# with GEGL's Python bindings, for when the gegl program (the gegl package
# on Debian and Ubuntu) is not installed: loads IN, applies OP with the
# properties and saves OUT. Like gegl, the property values are parsed by
# their type in the operation (numbers, booleans, enums by name, strings).
#
#   tests/gegl-cli.py IN OUT OP [KEY=VALUE...]
import sys

import gi
gi.require_version('Gegl', '0.4')
from gi.repository import Gegl, GObject

Gegl.init(None)
src, dst, op = sys.argv[1:4]
graph = Gegl.Node()
load = graph.create_child('gegl:load')
load.set_property('path', src)
node = graph.create_child(op)
if node.get_operation() != op:
    sys.exit('no operation %s' % op)
for arg in sys.argv[4:]:
    key, value = arg.split('=', 1)
    pspec = next((p for p in Gegl.Operation.list_properties(op)
                  if p.name == key), None)
    if pspec is None:
        sys.exit('%s has no property %s' % (op, key))
    t = pspec.value_type
    if t in (GObject.TYPE_DOUBLE, GObject.TYPE_FLOAT):
        v = float(value)
    elif t in (GObject.TYPE_INT, GObject.TYPE_UINT, GObject.TYPE_INT64,
               GObject.TYPE_UINT64):
        v = int(value)
    elif t == GObject.TYPE_BOOLEAN:
        v = value.lower() in ('1', 'true', 'yes')
    elif t.is_a(GObject.TYPE_ENUM):
        v = next(e.value for e in pspec.enum_class.values.values()
                 if value in (e.value_nick, e.value_name))
    else:
        v = value
    node.set_property(key, v)
save = graph.create_child('gegl:save')
save.set_property('path', dst)
load.link(node)
node.link(save)
save.process()
