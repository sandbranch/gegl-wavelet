/*
 * Wavelet denoise algorithm, shared by the GEGL operation
 *
 * denoise-core.c
 * Copyright 2008 by Marco Rossini (wavelet denoise GIMP plugin)
 *
 * Implements the wavelet denoise code of UFRaw by Udi Fuchs
 * which itself bases on the code by Dave Coffin
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * The colour model conversions and the wavelet transform are those of
 * the plugin files colorspace.c and wavelet.c. The progress reporting to
 * GIMP is left out, and the steps of the transform are spread over GEGL's
 * threads without changing the arithmetic; for that, the temporaries of
 * ycbcr2srgb are no longer static.
 */

#include "denoise-core.h"

void
srgb2ycbcr (float ** fimg, int size)
{
  /* using JPEG conversion here - expecting all channels to be
   * in [0:255] range */
  int i;
  float y, cb, cr;

  for (i = 0; i < size; i++) {
    y =   0.2990 * fimg[0][i] + 0.5870 * fimg[1][i] + 0.1140 * fimg[2][i];
    cb = -0.1687 * fimg[0][i] - 0.3313 * fimg[1][i] + 0.5000 * fimg[2][i]
         + 0.5;
    cr =  0.5000 * fimg[0][i] - 0.4187 * fimg[1][i] - 0.0813 * fimg[2][i]
         + 0.5;
    fimg[0][i] = y;
    fimg[1][i] = cb;
    fimg[2][i] = cr;
  }
}

void
ycbcr2srgb (float **fimg, int size, int pc)
{
  /* using JPEG conversion here - expecting all channels to be
   * in [0:255] range */
  int i;
  float r, g, b;

  if (pc > 3) { /* single channel, colour */
    pc -= 4;
    for (i = 0; i < size; i++) {
      fimg[(pc + 1) % 3][i] = 0.5;
      fimg[(pc + 2) % 3][i] = 0.5;
    }
  } else if (pc > 0) { /* single channel, gray */
    pc -= 1;
    for (i = 0; i < size; i++) {
      fimg[(pc + 1) % 3][i] = fimg[pc][i];
      fimg[(pc + 2) % 3][i] = fimg[pc][i];
    }
    return;
  }

  for (i = 0; i < size; i++) {
    r = fimg[0][i] + 1.40200 * (fimg[2][i] - 0.5);
    g = fimg[0][i] - 0.34414 * (fimg[1][i] - 0.5)
        - 0.71414 * (fimg[2][i] - 0.5);
    b = fimg[0][i] + 1.77200 * (fimg[1][i] - 0.5);
    fimg[0][i] = r;
    fimg[1][i] = g;
    fimg[2][i] = b;
  }
}

void
srgb2xyz (float **fimg, int size)
{
  /* fimg in [0:1], sRGB */
  int i;
  float x, y, z;

  for (i = 0; i < size; i++) {
    /* scaling and gamma correction (approximate) */
    fimg[0][i] = pow(fimg[0][i], 2.2);
    fimg[1][i] = pow(fimg[1][i], 2.2);
    fimg[2][i] = pow(fimg[2][i], 2.2);
 

    /* matrix RGB -> XYZ, with D65 reference white (www.brucelindbloom.com) */
    x = 0.412424 * fimg[0][i] + 0.357579 * fimg[1][i] + 0.180464 * fimg[2][i];
    y = 0.212656 * fimg[0][i] + 0.715158 * fimg[1][i] + 0.0721856 * fimg[2][i];
    z = 0.0193324 * fimg[0][i] + 0.119193 * fimg[1][i] + 0.950444 * fimg[2][i];

    /*
    x = 0.412424 * fimg[0][i] + 0.212656 * fimg[1][i] + 0.0193324 * fimg[2][i];
    y = 0.357579 * fimg[0][i] + 0.715158 * fimg[1][i] + 0.119193  * fimg[2][i];
    z = 0.180464 * fimg[0][i] + 0.0721856 * fimg[1][i] + 0.950444 * fimg[2][i];
    */

    fimg[0][i] = x;
    fimg[1][i] = y;
    fimg[2][i] = z;
  }
}

void
xyz2srgb (float **fimg, int size, int pc)
{
  int i;
  float r, g, b;

  if (pc > 3) { /* single channel, colour */
    pc -= 4;
    for (i = 0; i < size; i++) {
      fimg[(pc + 1) % 3][i] = 0.0;
      fimg[(pc + 2) % 3][i] = 0.0;
    }
  } else if (pc > 0) { /* single channel, gray */
    pc -= 1;
    for (i = 0; i < size; i++) {
      fimg[pc][i] = pow(fimg[pc][i], 1 / 2.2);
      fimg[(pc + 1) % 3][i] = fimg[pc][i];
      fimg[(pc + 2) % 3][i] = fimg[pc][i];
    }
    return;
  }

  for (i = 0; i < size; i++) {
    /* matrix RGB -> XYZ, with D65 reference white (www.brucelindbloom.com) */
    r = 3.24071 * fimg[0][i] - 1.53726 * fimg[1][i] - 0.498571 * fimg[2][i];
    g = -0.969258 * fimg[0][i] + 1.87599 * fimg[1][i] + 0.0415557 * fimg[2][i];
    b = 0.0556352 * fimg[0][i] - 0.203996 * fimg[1][i] + 1.05707 * fimg[2][i];

    /*
    r =  3.24071  * fimg[0][i] - 0.969258  * fimg[1][i]
      + 0.0556352 * fimg[2][i];
    g = -1.53726  * fimg[0][i] + 1.87599   * fimg[1][i]
      - 0.203996  * fimg[2][i];
    b = -0.498571 * fimg[0][i] + 0.0415557 * fimg[1][i]
      + 1.05707   * fimg[2][i];
    */
  
    /* scaling and gamma correction (approximate) */
    r = r < 0 ? 0 : pow(r, 1.0 / 2.2);
    g = g < 0 ? 0 : pow(g, 1.0 / 2.2);
    b = b < 0 ? 0 : pow(b, 1.0 / 2.2);
  
    fimg[0][i] = r;
    fimg[1][i] = g;
    fimg[2][i] = b;
  }
}

void lab2srgb (float **fimg, int size, int pc)
{
  int i;
  float x, y, z;
  float neutral [] = {0.5, 0.5, 0.5};

  if (pc > 3) { /* single channel, colour */
    pc -= 4;
    for (i = 0; i < size; i++) {
      fimg[(pc + 1) % 3][i] = neutral[(pc + 1) % 3];
      fimg[(pc + 2) % 3][i] = neutral[(pc + 2) % 3];
    }
  } else if (pc > 0) { /* single channel, gray */
    pc -= 1;
    for (i = 0; i < size; i++) {
      fimg[pc][i] = pow(fimg[pc][i], 1 / 2.2);
      fimg[(pc + 1) % 3][i] = fimg[pc][i];
      fimg[(pc + 2) % 3][i] = fimg[pc][i];
    }
    return;
  }

  for (i = 0; i < size; i++) {
    /* convert back to normal LAB */
    fimg[0][i] = (fimg[0][i] - 0 * 16 * 27 / 24389.0) * 116;
    fimg[1][i] = (fimg[1][i] - 0.5) * 500 * 2;
    fimg[2][i] = (fimg[2][i] - 0.5) * 200 * 2.2;

    /* matrix */
    y = (fimg[0][i] + 16) / 116;
    z = y - fimg[2][i] / 200.0;
    x = fimg[1][i] / 500.0 + y;

    /* scale */
    if (x * x * x > 216 / 24389.0)
      x = x * x * x;
    else
      x = (116 * x - 16) * 27 / 24389.0;
    if (fimg[0][i] > 216 / 27.0)
      y = y * y * y;
    else
      //y = fimg[0][i] * 27 / 24389.0;
      y = (116 * y - 16) * 27 / 24389.0;
    if (z * z * z > 216 / 24389.0)
      z = z * z * z;
    else
      z = (116 * z - 16) * 27 / 24389.0;

    /* white reference */
    fimg[0][i] = x * 0.95047;
    fimg[1][i] = y;
    fimg[2][i] = z * 1.08883;
  }
  xyz2srgb(fimg, size, 0);
}

void srgb2lab (float **fimg, int size)
{
  int i;
  float l, a, b;
  srgb2xyz(fimg, size);
  for (i = 0; i < size; i++) {
    /* reference white */
    fimg[0][i] /= 0.95047;
    /* (just for completeness)
    fimg[1][i] /= 1.00000; */
    fimg[2][i] /= 1.08883;

    /* scale */
    if (fimg[0][i] > 216 / 24389.0) {
      fimg[0][i] = pow(fimg[0][i], 1 / 3.0);
    } else {
      fimg[0][i] = (24389 * fimg[0][i] / 27.0 + 16) / 116.0;
    }
    if (fimg[1][i] > 216 / 24389.0) {
      fimg[1][i] = pow(fimg[1][i], 1 / 3.0);
    } else {
      fimg[1][i] = (24389 * fimg[1][i] / 27.0 + 16) / 116.0;
    }
    if (fimg[2][i] > 216 / 24389.0) {
      fimg[2][i] = pow(fimg[2][i], 1 / 3.0);
    } else {
      fimg[2][i] = (24389 * fimg[2][i] / 27.0 + 16) / 116.0;
    }

    l = 116 * fimg[1][i] - 16;
    a = 500 * (fimg[0][i] - fimg[1][i]);
    b = 200 * (fimg[1][i] - fimg[2][i]);
    fimg[0][i] = l / 116.0; // + 16 * 27 / 24389.0;
    fimg[1][i] = a / 500.0 / 2.0 + 0.5;
    fimg[2][i] = b / 200.0 / 2.2 + 0.5;
    if (fimg[0][i] < 0)
      fimg[0][i] = 0;
  }
}

void srgb2rgb(float **fimg, int size)
{
  /*int i;
  for (i = 0; i < size; i++)
    {
      fimg[0][i] = pow(fimg[0][i], 1.1);
      fimg[0][i] = pow(fimg[1][i], 1.1);
      fimg[0][i] = pow(fimg[2][i], 1.1);
    }*/
}

void
rgb2srgb (float **fimg, int size, int pc)
{
  int i;

  if (pc > 3) { /* single channel, colour */
    pc -= 4;
    for (i = 0; i < size; i++) {
      fimg[(pc + 1) % 3][i] = 0.0;
      fimg[(pc + 2) % 3][i] = 0.0;
    }
  } else if (pc > 0) { /* single channel, gray */
    pc -= 1;
    for (i = 0; i < size; i++) {
      /* fimg[pc][i] = pow(fimg[pc][i], 1 / 1.1); */
      fimg[(pc + 1) % 3][i] = fimg[pc][i];
      fimg[(pc + 2) % 3][i] = fimg[pc][i];
    }
    return;
  }

  /*for (i = 0; i < size; i++) {
    fimg[0][i] = pow(fimg[0][i], 1 / 1.1);
    fimg[1][i] = pow(fimg[1][i], 1 / 1.1);
    fimg[2][i] = pow(fimg[2][i], 1 / 1.1);
  }*/
}

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

/* code copied from UFRaw (which originates from dcraw) */
static void
hat_transform (float *temp, float *base, int st, int size, int sc)
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

/* The steps of one wavelet level are spread over GEGL's threads where each
   row, column or pixel is independent, so the result is the same as when
   running serially. The noise statistics are summed serially, since a
   different order of the sums could change their last digits. */

/* columns processed together, so that they are read row by row */
#define COLUMN_BLOCK 16

typedef struct
{
  float **fimg;
  int width, height, sc, hpass, lpass;
  float threshold;
  double low;
  const double *stdev;
} level_data;

static void
rows_pass (gsize offset, gsize count, gpointer user_data)
{
  level_data *d = user_data;
  float *temp = g_new (float, d->width);
  gsize row;
  int col;

  for (row = offset; row < offset + count; row++)
    {
      hat_transform (temp, d->fimg[d->hpass] + row * d->width, 1, d->width,
		     d->sc);
      for (col = 0; col < d->width; col++)
	d->fimg[d->lpass][row * d->width + col] = temp[col] * 0.25;
    }
  g_free (temp);
}

static void
columns_pass (gsize offset, gsize count, gpointer user_data)
{
  level_data *d = user_data;
  float *temp = g_new (float, d->height);
  float *block = g_new (float, (gsize) COLUMN_BLOCK * d->height);
  float *base = d->fimg[d->lpass];
  gsize first, n, b;
  int row;

  for (first = offset * COLUMN_BLOCK;
       first < (offset + count) * COLUMN_BLOCK && first < (gsize) d->width;
       first += COLUMN_BLOCK)
    {
      n = MIN2 ((gsize) COLUMN_BLOCK, d->width - first);

      /* copy the columns of the block, reading row by row */
      for (row = 0; row < d->height; row++)
	for (b = 0; b < n; b++)
	  block[b * d->height + row] = base[row * d->width + first + b];

      for (b = 0; b < n; b++)
	{
	  hat_transform (temp, block + b * d->height, 1, d->height, d->sc);
	  for (row = 0; row < d->height; row++)
	    base[row * d->width + first + b] = temp[row] * 0.25;
	}
    }
  g_free (block);
  g_free (temp);
}

static void
difference_pass (gsize offset, gsize count, gpointer user_data)
{
  level_data *d = user_data;
  gsize i;

  for (i = offset; i < offset + count; i++)
    d->fimg[d->hpass][i] -= d->fimg[d->lpass][i];
}

static void
threshold_pass (gsize offset, gsize count, gpointer user_data)
{
  level_data *d = user_data;
  float **fimg = d->fimg, thold;
  const double *stdev = d->stdev;
  int hpass = d->hpass, lpass = d->lpass;
  gsize i;

  for (i = offset; i < offset + count; i++)
    {
      if (fimg[lpass][i] > 0.8) {
	thold = d->threshold * stdev[4];
      } else if (fimg[lpass][i] > 0.6) {
	thold = d->threshold * stdev[3];
      } else if (fimg[lpass][i] > 0.4) {
	thold = d->threshold * stdev[2];
      } else if (fimg[lpass][i] > 0.2) {
	thold = d->threshold * stdev[1];
      } else {
	thold = d->threshold * stdev[0];
      }

      if (fimg[hpass][i] < -thold)
	fimg[hpass][i] += thold - thold * d->low;
      else if (fimg[hpass][i] > thold)
	fimg[hpass][i] -= thold - thold * d->low;
      else
	fimg[hpass][i] *= d->low;

      if (hpass)
	fimg[0][i] += fimg[hpass][i];
    }
}

static void
sum_pass (gsize offset, gsize count, gpointer user_data)
{
  level_data *d = user_data;
  gsize i;

  for (i = offset; i < offset + count; i++)
    d->fimg[0][i] = d->fimg[0][i] + d->fimg[d->lpass][i];
}

/* actual denoising algorithm. code copied from UFRaw (originates from dcraw) */
void
wavelet_denoise (float *fimg[3], unsigned int width,
		 unsigned int height, float threshold, double low, float a,
		 float b)
{
  float thold;
  unsigned int i, lev, lpass = 0, hpass, size;
  double stdev[5];
  unsigned int samples[5];
  level_data d;

  size = width * height;
  d.fimg = fimg;
  d.width = width;
  d.height = height;
  d.threshold = threshold;
  d.low = low;
  d.stdev = stdev;

  hpass = 0;
  for (lev = 0; lev < 5; lev++)
    {
      lpass = ((lev & 1) + 1);
      d.sc = 1 << lev;
      d.hpass = hpass;
      d.lpass = lpass;

      gegl_parallel_distribute_range (height, 4.0, rows_pass, &d);
      gegl_parallel_distribute_range ((width + COLUMN_BLOCK - 1)
				      / COLUMN_BLOCK, 1.0, columns_pass, &d);
      gegl_parallel_distribute_range (size, 1024.0, difference_pass, &d);

      thold =
	5.0 / (1 << 6) * exp (-2.6 * sqrt (lev + 1)) * 0.8002 / exp (-2.6);

      /* initialize stdev values for all intensities */
      stdev[0] = stdev[1] = stdev[2] = stdev[3] = stdev[4] = 0.0;
      samples[0] = samples[1] = samples[2] = samples[3] = samples[4] = 0;

      /* calculate stdevs for all intensities */
      for (i = 0; i < size; i++)
	{
	  if (fimg[hpass][i] < thold && fimg[hpass][i] > -thold)
	    {
	      if (fimg[lpass][i] > 0.8) {
	        stdev[4] += fimg[hpass][i] * fimg[hpass][i];
	        samples[4]++;
	      } else if (fimg[lpass][i] > 0.6) {
	        stdev[3] += fimg[hpass][i] * fimg[hpass][i];
	        samples[3]++;
	      }	else if (fimg[lpass][i] > 0.4) {
	        stdev[2] += fimg[hpass][i] * fimg[hpass][i];
	        samples[2]++;
	      }	else if (fimg[lpass][i] > 0.2) {
	        stdev[1] += fimg[hpass][i] * fimg[hpass][i];
	        samples[1]++;
	      } else {
	        stdev[0] += fimg[hpass][i] * fimg[hpass][i];
	        samples[0]++;
	      }
	    }
	}
      stdev[0] = sqrt (stdev[0] / (samples[0] + 1));
      stdev[1] = sqrt (stdev[1] / (samples[1] + 1));
      stdev[2] = sqrt (stdev[2] / (samples[2] + 1));
      stdev[3] = sqrt (stdev[3] / (samples[3] + 1));
      stdev[4] = sqrt (stdev[4] / (samples[4] + 1));

      /* do thresholding */
      gegl_parallel_distribute_range (size, 1024.0, threshold_pass, &d);
      hpass = lpass;
    }

  d.lpass = lpass;
  gegl_parallel_distribute_range (size, 1024.0, sum_pass, &d);
}
