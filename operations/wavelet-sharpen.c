/*
 * Wavelet sharpen GEGL operation
 *
 * wavelet-sharpen.c
 * Copyright 2008 by Marco Rossini (wavelet sharpen GIMP plugin)
 * GEGL operation 2026 by David
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2+
 * as published by the Free Software Foundation.
 *
 * The algorithm is the one of the wavelet sharpen GIMP plugin, which
 * copies code from UFRaw (which originates from dcraw).
 */

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

property_double (amount, _("Amount"), 0.0)
  description (_("Adjusts the amount of sharpening applied."))
  value_range (0.0, 10.0)
  ui_digits (1)

property_double (radius, _("Radius"), 0.5)
  description (_("Adjusts the radius of the sharpening. For very unsharp "
                 "images it is recommended to use higher values. Default is "
                 "0.5."))
  value_range (0.0, 2.0)
  ui_digits (2)

property_boolean (luminance, _("Sharpen luminance only"), TRUE)
  description (_("Sharpens luminance only (YCbCr luminance channel). This "
                 "avoids color artifacts to appear and makes the sharpening "
                 "faster. Colour sharpness in natural images is not critical "
                 "for the human eye."))

#else

#define GEGL_OP_AREA_FILTER
#define GEGL_OP_NAME     wavelet_sharpen
#define GEGL_OP_C_SOURCE wavelet-sharpen.c

#include "gegl-op.h"

/* number of wavelet levels, each doubling the scale of the last */
#define LEVELS 5

/* pixels a result depends on in each direction: 1 + 2 + 4 + 8 + 16 */
#define REACH ((1 << LEVELS) - 1)

/* index i mirrored at the borders 0 and size - 1, as often as needed */
static int
mirror (int i, int size)
{
  int period = 2 * (size - 1);

  if (period == 0)
    return 0;
  i = abs (i) % period;
  return i < size ? i : period - i;
}

static void
hat_transform (float *temp, float *base, gsize st, int size, int sc)
{
  int i;

  /* sizes too small for a single mirroring at the borders */
  if (size < 2 * sc)
    {
      for (i = 0; i < size; i++)
        temp[i] = 2 * base[st * i] + base[st * mirror (i - sc, size)]
          + base[st * mirror (i + sc, size)];
      return;
    }

  for (i = 0; i < sc; i++)
    temp[i] = 2 * base[st * i] + base[st * (sc - i)] + base[st * (i + sc)];
  for (; i + sc < size; i++)
    temp[i] = 2 * base[st * i] + base[st * (i - sc)] + base[st * (i + sc)];
  for (; i < size; i++)
    temp[i] = 2 * base[st * i] + base[st * (i - sc)]
      + base[st * (2 * size - 2 - (i + sc))];
}

/* the sharpening of the plugin, on fimg[0] with fimg[1] and fimg[2] as
   work space */
static void
wavelet_sharpen (float *fimg[3], int width, int height,
                 double amount, double radius)
{
  float *temp, amt;
  int lev, lpass = 0, hpass, col, row;
  gsize i, size;

  size = (gsize) width * height;
  temp = g_new (float, MAX (width, height));

  hpass = 0;
  for (lev = 0; lev < LEVELS; lev++)
    {
      lpass = ((lev & 1) + 1);
      for (row = 0; row < height; row++)
        {
          hat_transform (temp, fimg[hpass] + (gsize) row * width, 1, width,
                         1 << lev);
          for (col = 0; col < width; col++)
            fimg[lpass][(gsize) row * width + col] = temp[col] * 0.25;
        }
      for (col = 0; col < width; col++)
        {
          hat_transform (temp, fimg[lpass] + col, width, height, 1 << lev);
          for (row = 0; row < height; row++)
            fimg[lpass][(gsize) row * width + col] = temp[row] * 0.25;
        }

      amt = amount * exp (-(lev - radius) * (lev - radius) / 1.5) + 1;
      for (i = 0; i < size; i++)
        {
          fimg[hpass][i] -= fimg[lpass][i];
          fimg[hpass][i] *= amt;

          if (hpass)
            fimg[0][i] += fimg[hpass][i];
        }
      hpass = lpass;
    }

  for (i = 0; i < size; i++)
    fimg[0][i] = fimg[0][i] + fimg[lpass][i];

  g_free (temp);
}

/* JPEG conversion, all channels in [0:255] as in the plugin */
static void
rgb2ycbcr (float *r, float *g, float *b)
{
  float y, cb, cr;

  y = 0.2990 * *r + 0.5870 * *g + 0.1140 * *b;
  cb = -0.1687 * *r - 0.3313 * *g + 0.5000 * *b + 128.0;
  cr = 0.5000 * *r - 0.4187 * *g - 0.0813 * *b + 128.0;
  *r = y;
  *g = cb;
  *b = cr;
}

static void
ycbcr2rgb (float *y, float *cb, float *cr)
{
  float r, g, b;

  r = *y + 1.40200 * (*cr - 128.0);
  g = *y - 0.34414 * (*cb - 128.0) - 0.71414 * (*cr - 128.0);
  b = *y + 1.77200 * (*cb - 128.0);
  *y = r;
  *cb = g;
  *cr = b;
}

static void
prepare (GeglOperation *operation)
{
  GeglOperationAreaFilter *area = GEGL_OPERATION_AREA_FILTER (operation);
  const Babl *space = gegl_operation_get_source_space (operation, "input");
  const Babl *format = babl_format_with_space ("R'G'B'A float", space);

  area->left = area->right = area->top = area->bottom = REACH;

  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);
}

/* the result covers the input only, not the reach around it */
static GeglRectangle
get_bounding_box (GeglOperation *operation)
{
  const GeglRectangle *in = gegl_operation_source_get_bounding_box (operation,
                                                                     "input");

  return in ? *in : *GEGL_RECTANGLE (0, 0, 0, 0);
}

static gboolean
process (GeglOperation       *operation,
         GeglBuffer          *input,
         GeglBuffer          *output,
         const GeglRectangle *result,
         gint                 level)
{
  GeglProperties *o = GEGL_PROPERTIES (operation);
  const Babl *format = gegl_operation_get_format (operation, "output");
  const GeglRectangle *bounds;
  GeglRectangle src, dst;
  float *pixels, *fimg[3], *work[3];
  gint c, channels;
  gsize i, size;

  /* the area around the result, mirrored at the borders of the image like
     the plugin does at the borders of the selection */
  src = *result;
  src.x -= REACH;
  src.y -= REACH;
  src.width += 2 * REACH;
  src.height += 2 * REACH;
  bounds = gegl_operation_source_get_bounding_box (operation, "input");
  if (bounds)
    gegl_rectangle_intersect (&src, &src, bounds);
  if (src.width <= 0 || src.height <= 0)
    return TRUE;

  size = (gsize) src.width * src.height;
  pixels = g_new (float, size * 4);
  for (c = 0; c < 3; c++)
    fimg[c] = g_new (float, size);
  work[1] = g_new (float, size);
  work[2] = g_new (float, size);

  gegl_buffer_get (input, &src, 1.0, format, pixels, GEGL_AUTO_ROWSTRIDE,
                   GEGL_ABYSS_NONE);

  /* colour model conversion in [0:255], scaled to [0:1] for sharpening */
  for (i = 0; i < size; i++)
    {
      float val[3];

      for (c = 0; c < 3; c++)
        val[c] = pixels[i * 4 + c] * 255.0;
      if (o->luminance)
        rgb2ycbcr (&val[0], &val[1], &val[2]);
      for (c = 0; c < 3; c++)
        fimg[c][i] = val[c] / 255.0;
    }

  /* the alpha channel is never sharpened */
  channels = o->luminance ? 1 : 3;
  for (c = 0; c < channels; c++)
    {
      work[0] = fimg[c];
      wavelet_sharpen (work, src.width, src.height, o->amount, o->radius);
    }

  for (i = 0; i < size; i++)
    {
      float val[3];

      for (c = 0; c < 3; c++)
        val[c] = fimg[c][i] * 255.0;
      if (o->luminance)
        ycbcr2rgb (&val[0], &val[1], &val[2]);
      for (c = 0; c < 3; c++)
        pixels[i * 4 + c] = val[c] / 255.0;
    }

  /* write the part of the area that was asked for and lies in the image */
  if (gegl_rectangle_intersect (&dst, result, &src))
    gegl_buffer_set (output, &dst, 0, format,
                     pixels + ((gsize) (dst.y - src.y) * src.width
                               + (dst.x - src.x)) * 4,
                     src.width * 4 * sizeof (float));

  g_free (pixels);
  for (c = 0; c < 3; c++)
    g_free (fimg[c]);
  g_free (work[1]);
  g_free (work[2]);

  return TRUE;
}

/* the input is passed on untouched when there is nothing to sharpen */
static gboolean
operation_process (GeglOperation        *operation,
                   GeglOperationContext *context,
                   const gchar          *output_prop,
                   const GeglRectangle  *result,
                   gint                  level)
{
  GeglOperationClass *operation_class;
  GeglProperties *o = GEGL_PROPERTIES (operation);

  operation_class = GEGL_OPERATION_CLASS (gegl_op_parent_class);

  if (o->amount <= 0.0)
    {
      gpointer in = gegl_operation_context_get_object (context, "input");

      gegl_operation_context_take_object (context, "output",
                                          in ? g_object_ref (in) : NULL);
      return TRUE;
    }

  return operation_class->process (operation, context, output_prop, result,
                                   gegl_operation_context_get_level (context));
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GeglOperationClass *operation_class = GEGL_OPERATION_CLASS (klass);
  GeglOperationFilterClass *filter_class = GEGL_OPERATION_FILTER_CLASS (klass);

  operation_class->prepare = prepare;
  operation_class->process = operation_process;
  operation_class->get_bounding_box = get_bounding_box;
  filter_class->process = process;

  gegl_operation_class_set_keys (operation_class,
    "name",            "wavelet:sharpen",
    "title",           _("Wavelet Sharpen"),
    "categories",      "enhance:sharpen",
    "description",     _("Sharpens the image using wavelets."),
    "gimp:menu-path",  "<Image>/Filters/Enhance",
    "gimp:menu-label", _("Wavelet Sharpen..."),
    NULL);
}

#endif
