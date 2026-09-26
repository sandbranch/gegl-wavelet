/*
 * Wavelet denoise algorithm, shared by the GEGL operation
 *
 * denoise-core.h
 * Copyright 2008 by Marco Rossini (wavelet denoise GIMP plugin)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#ifndef __DENOISE_CORE_H__
#define __DENOISE_CORE_H__

#include <stdlib.h>
#include <math.h>
#include <gegl.h>

#define MAX2(x,y) ((x) > (y) ? (x) : (y))
#define MIN2(x,y) ((x) < (y) ? (x) : (y))
#define CLIP(x,min,max) MAX2((min), MIN2((x), (max)))

/* denoise fimg[0] with fimg[1] and fimg[2] as work space */
void wavelet_denoise (float *fimg[3], unsigned int width,
                      unsigned int height, float threshold, double low);

void srgb2rgb (float **fimg, gsize size);
void rgb2srgb (float **fimg, gsize size, int pc);
void srgb2ycbcr (float **fimg, gsize size);
void ycbcr2srgb (float **fimg, gsize size, int pc);
void srgb2lab (float **fimg, gsize size);
void lab2srgb (float **fimg, gsize size, int pc);
void srgb2xyz (float **fimg, gsize size);
void xyz2srgb (float **fimg, gsize size, int pc);

#endif /* __DENOISE_CORE_H__ */
