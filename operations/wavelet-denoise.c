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

property_double (threshold_1, _("Threshold 1"), 0.0)
  description (_("The value below which everything is considered noise, from 0.0 "
                 "(none) to 10.0. For grayscale images only the first channel "
                 "is used."))
  value_range (0.0, 10.0)
  ui_digits (2)
  ui_meta ("label", "[color-model {ycbcr} : ycbcr-label,"
                    " color-model {lab} : lab-label,"
                    " color-model {rgb} : rgb-label]")
  ui_meta ("ycbcr-label", _("Luminance (Y) threshold"))
  ui_meta ("lab-label", _("Lightness (L*) threshold"))
  ui_meta ("rgb-label", _("Red threshold"))
property_double (softness_1, _("Softness 1"), 0.0)
  description (_("Softness of the thresholding. The higher the softness the "
                 "more noise remains."))
  value_range (0.0, 1.0)
  ui_digits (2)
  ui_meta ("label", "[color-model {ycbcr} : ycbcr-label,"
                    " color-model {lab} : lab-label,"
                    " color-model {rgb} : rgb-label]")
  ui_meta ("ycbcr-label", _("Luminance (Y) softness"))
  ui_meta ("lab-label", _("Lightness (L*) softness"))
  ui_meta ("rgb-label", _("Red softness"))

property_double (threshold_2, _("Threshold 2"), 0.4)
  description (_("The value below which everything is considered noise."))
  value_range (0.0, 10.0)
  ui_digits (2)
  ui_meta ("label", "[color-model {ycbcr} : ycbcr-label,"
                    " color-model {lab} : lab-label,"
                    " color-model {rgb} : rgb-label]")
  ui_meta ("ycbcr-label", _("Blue chroma (Cb) threshold"))
  ui_meta ("lab-label", _("Green-red (a*) threshold"))
  ui_meta ("rgb-label", _("Green threshold"))
property_double (softness_2, _("Softness 2"), 0.0)
  description (_("Softness of the thresholding. The higher the softness the "
                 "more noise remains."))
  value_range (0.0, 1.0)
  ui_digits (2)
  ui_meta ("label", "[color-model {ycbcr} : ycbcr-label,"
                    " color-model {lab} : lab-label,"
                    " color-model {rgb} : rgb-label]")
  ui_meta ("ycbcr-label", _("Blue chroma (Cb) softness"))
  ui_meta ("lab-label", _("Green-red (a*) softness"))
  ui_meta ("rgb-label", _("Green softness"))

property_double (threshold_3, _("Threshold 3"), 0.4)
  description (_("The value below which everything is considered noise."))
  value_range (0.0, 10.0)
  ui_digits (2)
  ui_meta ("label", "[color-model {ycbcr} : ycbcr-label,"
                    " color-model {lab} : lab-label,"
                    " color-model {rgb} : rgb-label]")
  ui_meta ("ycbcr-label", _("Red chroma (Cr) threshold"))
  ui_meta ("lab-label", _("Blue-yellow (b*) threshold"))
  ui_meta ("rgb-label", _("Blue threshold"))
property_double (softness_3, _("Softness 3"), 0.0)
  description (_("Softness of the thresholding. The higher the softness the "
                 "more noise remains."))
  value_range (0.0, 1.0)
  ui_digits (2)
  ui_meta ("label", "[color-model {ycbcr} : ycbcr-label,"
                    " color-model {lab} : lab-label,"
                    " color-model {rgb} : rgb-label]")
  ui_meta ("ycbcr-label", _("Red chroma (Cr) softness"))
  ui_meta ("lab-label", _("Blue-yellow (b*) softness"))
  ui_meta ("rgb-label", _("Blue softness"))

property_boolean (denoise_alpha, _("Denoise alpha"), FALSE)
  description (_("Also denoise the alpha channel."))

property_double (threshold_alpha, _("Alpha threshold"), 0.0)
  description (_("Threshold of the alpha channel."))
  value_range (0.0, 10.0)
  ui_digits (2)
  ui_meta ("visible", "denoise-alpha")
property_double (softness_alpha, _("Alpha softness"), 0.0)
  description (_("Softness of the thresholding of the alpha channel."))
  value_range (0.0, 1.0)
  ui_digits (2)
  ui_meta ("visible", "denoise-alpha")

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
   of the input, and is computed once for all of it; an input without
   bounds, such as a pattern, is passed through */
static gboolean
get_area (GeglOperation *operation,
          GeglRectangle *area)
{
  const GeglRectangle *in = gegl_operation_source_get_bounding_box (operation,
                                                                     "input");

  if (!in || gegl_rectangle_is_infinite_plane (in))
    return FALSE;
  *area = *in;
  return TRUE;
}

static GeglRectangle
get_required_for_output (GeglOperation       *operation,
                         const gchar         *input_pad,
                         const GeglRectangle *roi)
{
  GeglRectangle area;

  return get_area (operation, &area) ? area : *roi;
}

static GeglRectangle
get_cached_region (GeglOperation       *operation,
                   const GeglRectangle *roi)
{
  GeglRectangle area;

  return get_area (operation, &area) ? area : *roi;
}

/* the thresholds of the colour channels and alpha, in the order of the
   channels of the format; FALSE if none of them denoises */
static gboolean
get_settings (GeglProperties *o,
              gint            channels,
              double          thresholds[4],
              double          low[4])
{
  double alpha_threshold = o->denoise_alpha ? o->threshold_alpha : 0.0;
  gint c;

  thresholds[0] = o->threshold_1;
  thresholds[1] = channels > 2 ? o->threshold_2 : alpha_threshold;
  thresholds[2] = o->threshold_3;
  thresholds[3] = alpha_threshold;
  low[0] = o->softness_1;
  low[1] = channels > 2 ? o->softness_2 : o->softness_alpha;
  low[2] = o->softness_3;
  low[3] = o->softness_alpha;

  for (c = 0; c < channels; c++)
    if (thresholds[c] > 0)
      return TRUE;
  return FALSE;
}

/* the input is passed on untouched when nothing is denoised: the colour
   model conversions alone would change it slightly */
static gboolean
operation_process (GeglOperation        *operation,
                   GeglOperationContext *context,
                   const gchar          *output_prop,
                   const GeglRectangle  *result,
                   gint                  level)
{
  GeglOperationClass *operation_class;
  const Babl *format = gegl_operation_get_format (operation, "output");
  double thresholds[4], low[4];
  GeglRectangle area;

  operation_class = GEGL_OPERATION_CLASS (gegl_op_parent_class);

  if (!get_area (operation, &area)
      || !get_settings (GEGL_PROPERTIES (operation),
                        babl_format_get_n_components (format), thresholds,
                        low))
    {
      gpointer in = gegl_operation_context_get_object (context, "input");

      gegl_operation_context_take_object (context, "output",
                                          in ? g_object_ref (in) : NULL);
      return TRUE;
    }

  return operation_class->process (operation, context, output_prop, result,
                                   gegl_operation_context_get_level (context));
}

/* the colour model conversions work pixel by pixel, so they are spread
   over GEGL's threads in ranges of pixels */
typedef struct
{
  float **fimg;
  void (*to_model) (float **fimg, gsize size);
  void (*from_model) (float **fimg, gsize size, int pc);
} convert_data;

static void
convert_range (gsize offset, gsize count, gpointer user_data)
{
  convert_data *c = user_data;
  float *part[3] = { c->fimg[0] + offset, c->fimg[1] + offset,
                     c->fimg[2] + offset };

  if (c->to_model)
    c->to_model (part, count);
  else
    c->from_model (part, count, 0);
}

static void
convert (float **fimg, gsize size, gint model, gboolean to_model)
{
  convert_data c = { fimg, NULL, NULL };

  if (to_model)
    c.to_model = model == WAVELET_DENOISE_YCBCR ? srgb2ycbcr
      : model == WAVELET_DENOISE_LAB ? srgb2lab : srgb2rgb;
  else
    c.from_model = model == WAVELET_DENOISE_YCBCR ? ycbcr2srgb
      : model == WAVELET_DENOISE_LAB ? lab2srgb : rgb2srgb;

  gegl_parallel_distribute_range (size, 1024.0, convert_range, &c);
}

static void
free_planes (float **planes, gint n)
{
  gint c;

  for (c = 0; c < n; c++)
    g_free (planes[c]);
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
  GeglRectangle area, strip_rect;
  double thresholds[4], low[4];
  float *fimg[4] = { NULL, NULL, NULL, NULL };
  float *work[3] = { NULL, NULL, NULL };
  float *strip = NULL;
  gint channels, c, y, rows, width, height;
  gboolean alloc_ok;
  gsize i, size;

  if (!get_area (operation, &area))
    area = *result;
  width = area.width;
  height = area.height;
  if (width <= 0 || height <= 0)
    return TRUE;

  /* the colour channels plus alpha, whose settings are the last ones */
  channels = babl_format_get_n_components (format);
  get_settings (o, channels, thresholds, low);

  /* one plane per channel plus two for the wavelet transform, as much as
     the plugin needs; a failure leaves the image as it is instead of
     ending GIMP */
  size = (gsize) width * height;
  alloc_ok = TRUE;
  for (c = 0; c < channels; c++)
    alloc_ok = alloc_ok && (fimg[c] = g_try_new (float, size)) != NULL;
  alloc_ok = alloc_ok && (work[1] = g_try_new (float, size)) != NULL;
  alloc_ok = alloc_ok && (work[2] = g_try_new (float, size)) != NULL;
  alloc_ok = alloc_ok && (strip = g_try_new (float, (gsize) width
                                             * STRIP_HEIGHT * channels)) != NULL;
  if (!alloc_ok)
    {
      g_warning ("wavelet:denoise: not enough memory for %d x %d pixels, "
                 "the image is left as it is", width, height);
      gegl_buffer_copy (input, &area, GEGL_ABYSS_NONE, output, &area);
      free_planes (fimg, channels);
      free_planes (work + 1, 2);
      return TRUE;
    }

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
    convert (fimg, size, o->color_model, TRUE);

  /* denoise the channels individually */
  for (c = 0; c < channels; c++)
    {
      if (thresholds[c] <= 0)
        continue;
      work[0] = fimg[c];
      wavelet_denoise (work, width, height, (float) thresholds[c], low[c]);
    }

  /* retransform the image data */
  if (channels > 2)
    convert (fimg, size, o->color_model, FALSE);

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
  free_planes (fimg, channels);
  free_planes (work + 1, 2);

  return TRUE;
}

static void
gegl_op_class_init (GeglOpClass *klass)
{
  GeglOperationClass *operation_class = GEGL_OPERATION_CLASS (klass);
  GeglOperationFilterClass *filter_class = GEGL_OPERATION_FILTER_CLASS (klass);

  operation_class->prepare = prepare;
  operation_class->process = operation_process;
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
