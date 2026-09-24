/*
 * Wavelet denoise GEGL operation
 *
 * wavelet-denoise.c
 * Copyright 2008 by Marco Rossini (wavelet denoise GIMP plugin)
 * GEGL operation 2026 by David
 *
 * Implements the wavelet denoise code of UFRaw by Udi Fuchs
 * which itself bases on the code by Dave Coffin
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#include <glib/gi18n-lib.h>

#ifdef GEGL_PROPERTIES

enum_start (wavelet_denoise_color_model)
  enum_value (WAVELET_DENOISE_YCBCR, "ycbcr", N_("YCbCr"))
  enum_value (WAVELET_DENOISE_LAB,   "lab",   N_("CIELAB"))
  enum_value (WAVELET_DENOISE_RGB,   "rgb",   N_("RGB"))
enum_end (WaveletDenoiseColorModel)

property_enum (color_model, _("Color model"),
               WaveletDenoiseColorModel, wavelet_denoise_color_model,
               WAVELET_DENOISE_YCBCR)
  description (_("Color model in which the channels are denoised. YCbCr "
                 "and CIELAB separate brightness from color, so that color "
                 "noise can be removed without touching detail. Ignored for "
                 "grayscale images."))

property_double (threshold_1, _("Threshold 1 (Y, L*, R, gray)"), 0.0)
  description (_("Threshold of the first channel, the value below which "
                 "everything is considered noise. 0 leaves it untouched."))
  value_range (0.0, 10.0)
  ui_digits (2)
property_double (softness_1, _("Softness 1 (Y, L*, R, gray)"), 0.0)
  description (_("Softness of the thresholding of the first channel. The "
                 "higher the softness the more noise remains."))
  value_range (0.0, 1.0)
  ui_digits (2)

property_double (threshold_2, _("Threshold 2 (Cb, a*, G)"), 0.4)
  description (_("Threshold of the second channel."))
  value_range (0.0, 10.0)
  ui_digits (2)
property_double (softness_2, _("Softness 2 (Cb, a*, G)"), 0.0)
  description (_("Softness of the thresholding of the second channel."))
  value_range (0.0, 1.0)
  ui_digits (2)

property_double (threshold_3, _("Threshold 3 (Cr, b*, B)"), 0.4)
  description (_("Threshold of the third channel."))
  value_range (0.0, 10.0)
  ui_digits (2)
property_double (softness_3, _("Softness 3 (Cr, b*, B)"), 0.0)
  description (_("Softness of the thresholding of the third channel."))
  value_range (0.0, 1.0)
  ui_digits (2)

property_double (threshold_alpha, _("Alpha threshold"), 0.0)
  description (_("Threshold of the alpha channel."))
  value_range (0.0, 10.0)
  ui_digits (2)
property_double (softness_alpha, _("Alpha softness"), 0.0)
  description (_("Softness of the thresholding of the alpha channel."))
  value_range (0.0, 1.0)
  ui_digits (2)

#else

#define GEGL_OP_FILTER
#define GEGL_OP_NAME     wavelet_denoise
#define GEGL_OP_C_SOURCE wavelet-denoise.c

#include "gegl-op.h"
#include "denoise-core.h"

/* number of rows transferred at once */
#define STRIP_HEIGHT 64

static gboolean
input_is_gray (GeglOperation *operation)
{
  const Babl *format = gegl_operation_get_source_format (operation, "input");

  return format && (babl_get_model_flags (format) & BABL_MODEL_FLAG_GRAY);
}

static void
prepare (GeglOperation *operation)
{
  const Babl *space = gegl_operation_get_source_space (operation, "input");
  const Babl *format;

  /* the algorithm works on non-linear values in [0,1], as it did on
     8-bit sRGB data in the plugin; grayscale stays grayscale */
  format = babl_format_with_space (input_is_gray (operation)
                                   ? "Y'A float" : "R'G'B'A float", space);

  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);
}

/* the noise is measured over the whole image, so every result needs all
   of the input, and is computed once for all of it */
static GeglRectangle
get_required_for_output (GeglOperation       *operation,
                         const gchar         *input_pad,
                         const GeglRectangle *roi)
{
  const GeglRectangle *in = gegl_operation_source_get_bounding_box (operation,
                                                                     "input");

  return in ? *in : *roi;
}

static GeglRectangle
get_cached_region (GeglOperation       *operation,
                   const GeglRectangle *roi)
{
  const GeglRectangle *in = gegl_operation_source_get_bounding_box (operation,
                                                                     "input");

  return in ? *in : *roi;
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
  GeglRectangle area, strip_rect;
  double thresholds[4], low[4];
  float *fimg[4] = { NULL, NULL, NULL, NULL };
  float *work[3] = { NULL, NULL, NULL };
  float *strip;
  gint channels, c, y, rows, width, height;
  gsize i, size;

  bounds = gegl_operation_source_get_bounding_box (operation, "input");
  area = bounds ? *bounds : *result;
  width = area.width;
  height = area.height;
  if (width <= 0 || height <= 0)
    return TRUE;

  /* the colour channels plus alpha, whose settings are the last ones */
  channels = babl_format_get_n_components (format);
  thresholds[0] = o->threshold_1;
  thresholds[1] = channels > 2 ? o->threshold_2 : o->threshold_alpha;
  thresholds[2] = o->threshold_3;
  thresholds[3] = o->threshold_alpha;
  low[0] = o->softness_1;
  low[1] = channels > 2 ? o->softness_2 : o->softness_alpha;
  low[2] = o->softness_3;
  low[3] = o->softness_alpha;

  size = (gsize) width * height;
  for (c = 0; c < channels; c++)
    fimg[c] = g_new (float, size);
  work[1] = g_new (float, size);
  work[2] = g_new (float, size);
  strip = g_new (float, (gsize) width * STRIP_HEIGHT * channels);

  for (y = 0; y < height; y += STRIP_HEIGHT)
    {
      rows = MIN2 (STRIP_HEIGHT, height - y);
      strip_rect = *GEGL_RECTANGLE (area.x, area.y + y, width, rows);
      gegl_buffer_get (input, &strip_rect, 1.0, format, strip,
                       GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
      for (i = 0; i < (gsize) rows * width; i++)
        for (c = 0; c < channels; c++)
          fimg[c][(gsize) y * width + i] = strip[i * channels + c];
    }

  /* do colour model conversion sRGB[0,1] -> whatever */
  if (channels > 2)
    {
      if (o->color_model == WAVELET_DENOISE_YCBCR)
        srgb2ycbcr (fimg, size);
      else if (o->color_model == WAVELET_DENOISE_LAB)
        srgb2lab (fimg, size);
      else
        srgb2rgb (fimg, size);
    }

  /* denoise the channels individually */
  for (c = 0; c < channels; c++)
    {
      if (thresholds[c] <= 0)
        continue;
      work[0] = fimg[c];
      wavelet_denoise (work, width, height, (float) thresholds[c], low[c],
                       0, 0);
    }

  /* retransform the image data */
  if (channels > 2)
    {
      if (o->color_model == WAVELET_DENOISE_YCBCR)
        ycbcr2srgb (fimg, size, 0);
      else if (o->color_model == WAVELET_DENOISE_LAB)
        lab2srgb (fimg, size, 0);
      else
        rgb2srgb (fimg, size, 0);
    }

  /* alpha stays in [0,1]; colour is limited by the precision of the
     image when GIMP stores the result, and floating point images keep
     values beyond it */
  for (i = 0; i < size; i++)
    fimg[channels - 1][i] = CLIP (fimg[channels - 1][i], 0.0, 1.0);

  for (y = 0; y < height; y += STRIP_HEIGHT)
    {
      rows = MIN2 (STRIP_HEIGHT, height - y);
      strip_rect = *GEGL_RECTANGLE (area.x, area.y + y, width, rows);
      for (i = 0; i < (gsize) rows * width; i++)
        for (c = 0; c < channels; c++)
          strip[i * channels + c] = fimg[c][(gsize) y * width + i];
      gegl_buffer_set (output, &strip_rect, 0, format, strip,
                       GEGL_AUTO_ROWSTRIDE);
    }

  g_free (strip);
  for (c = 0; c < channels; c++)
    g_free (fimg[c]);
  g_free (work[1]);
  g_free (work[2]);

  return TRUE;
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GeglOperationClass *operation_class = GEGL_OPERATION_CLASS (klass);
  GeglOperationFilterClass *filter_class = GEGL_OPERATION_FILTER_CLASS (klass);

  operation_class->prepare = prepare;
  operation_class->get_required_for_output = get_required_for_output;
  operation_class->get_cached_region = get_cached_region;
  operation_class->threaded = FALSE;
  filter_class->process = process;

  gegl_operation_class_set_keys (operation_class,
    "name",            "wavelet:denoise",
    "title",           _("Wavelet Denoise"),
    "categories",      "enhance:noise-reduction",
    "description",     _("Removes noise in the image using wavelets, with "
                         "separate settings for brightness and color "
                         "noise."),
    "gimp:menu-path",  "<Image>/Filters/Enhance",
    "gimp:menu-label", _("Wavelet Denoise..."),
    NULL);
}

#endif
