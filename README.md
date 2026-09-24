# gegl-wavelet

Marco Rossini's wavelet filters as GEGL operations, so they run in GIMP 3 as
non-destructive filters: they show up under Filters > Enhance, preview on the
canvas with split view, and stay editable in the layer's filter list (Fx)
until merged.

| Operation         | Status  | Based on |
|-------------------|---------|----------|
| `wavelet:sharpen` | working | [gimp-wavelet-sharpen](https://github.com/sandbranch/gimp-wavelet-sharpen) |
| `wavelet:denoise` | planned | [gimp-wavelet-denoise](https://github.com/sandbranch/gimp-wavelet-denoise) |

The same algorithms are also available as classic GIMP 3 plug-ins, which have
their own dialogs and can be called from scripts. The plug-ins apply their
result directly to the layer.

## Wavelet Sharpen

Settings: amount (0 to 10), radius (0 to 2) and whether to sharpen the
luminance only. The results equal those of the plug-in: identical on 8-bit
images and within one step on 16-bit images in our tests. The image is
processed as float in its own colour space, so any precision is kept. The alpha
channel is never sharpened.

## Building and installing

Needs meson, ninja, a C compiler and the GEGL development files (0.4.62 or
newer; libgegl-dev on Debian and Ubuntu).

GEGL loads user operations from `~/.local/share/gegl-0.4/plug-ins`:

    meson setup build -Dmoduledir=$HOME/.local/share/gegl-0.4/plug-ins
    ninja -C build install

### Flatpak GIMP

The Flatpak version of GIMP reads them from
`~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins`. Build against the GEGL of
the Flatpak, using the GNOME SDK that GIMP was built with (see
`flatpak info org.gimp.GIMP`):

    flatpak install --user flathub org.gnome.Sdk//50
    flatpak run --devel --filesystem=$PWD \
      --env=PKG_CONFIG_PATH=/app/lib/pkgconfig --command=sh org.gimp.GIMP -c \
      'meson setup build -Dmoduledir=$XDG_DATA_HOME/gegl-0.4/plug-ins &&
       ninja -C build install'

Restart GIMP after installing.

## License

GPL version 2 or later, like the wavelet sharpen plug-in. See COPYING.
