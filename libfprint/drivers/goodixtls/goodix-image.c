/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * Split out of goodix55x4.c. */
#include "goodix-image.h"
#include <math.h>
#include <string.h>

void
goodix_image_decode_frame (Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE],
                           const guint8 *raw_frame)
{

  Goodix55X4Pix *pix = frame;

  for (int i = 0; i < GOODIX55X4_RAW_FRAME_SIZE; i += 6)
    {
      const guint8 *chunk = raw_frame + i;
      *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
      *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
      *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
      *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
    }

}

/* Absolute per-pixel difference from the no-finger calibration image. */
void
goodix_image_postprocess_frame (Goodix55X4Pix       frame[GOODIX55X4_FRAME_SIZE],
                                const Goodix55X4Pix background[GOODIX55X4_FRAME_SIZE])
{
  for (int i = 0; i < GOODIX55X4_FRAME_SIZE; ++i)
    frame[i] = frame[i] > background[i] ? frame[i] - background[i] :
               background[i] - frame[i];
}

/* ---- swipe imaging helpers (ported from ElvinStarry/libfprint goodix55a2) ---- */

/* raw decoded mean over the full 56x176 frame (12-bit values) */
gint
goodix_image_swipe_raw_mean (const Goodix55X4Pix *frame)
{
  guint64 sum = 0;

  for (int i = 0; i < GOODIX55X4_FRAME_SIZE; ++i)
    sum += frame[i];
  return (gint) (sum / GOODIX55X4_FRAME_SIZE);
}

/* crop the 4-px borders and squash 12-bit -> 8-bit into an OUT_W x OUT_H buf */
void
goodix_image_swipe_build_out (const Goodix55X4Pix *frame, guint8 *out)
{
  for (int oy = 0; oy < GOODIX55X4_OUT_HEIGHT; ++oy)
    for (int ox = 0; ox < GOODIX55X4_OUT_WIDTH; ++ox)
      {
        const int sx = ox + GOODIX55X4_CROP;
        const int sy = oy + GOODIX55X4_CROP;
        guint v = frame[sx + sy * GOODIX55X4_WIDTH] >> 4;
        out[ox + oy * GOODIX55X4_OUT_WIDTH] = v > 255 ? 255 : (guint8) v;
      }
}

/* 4-channel fixed-pattern-noise correction + p10/p90 contrast stretch.
 * The sensor reads out 4 interleaved channels cycling across rows; equalise
 * each channel's mean to the global mean, then stretch the central histogram
 * so NBIS gets strong ridge/valley contrast. Operates in place on OUT_WxOUT_H. */
void
goodix_image_swipe_fpn_stretch (guint8 *img)
{
  const int w = GOODIX55X4_OUT_WIDTH, h = GOODIX55X4_OUT_HEIGHT;
  const gsize n = (gsize) w * h;

  double global_sum = 0;

  for (gsize i = 0; i < n; ++i)
    global_sum += img[i];
  const double global_mean = global_sum / n;

  for (int ch = 0; ch < 4; ++ch)
    {
      double ch_sum = 0;
      gsize ch_n = 0;
      for (int y = ch; y < h; y += 4)
        for (int x = 0; x < w; ++x)
          {
            ch_sum += img[y * w + x];
            ch_n++;
          }
      if (!ch_n)
        continue;
      const double offset = global_mean - (ch_sum / ch_n);
      for (int y = ch; y < h; y += 4)
        for (int x = 0; x < w; ++x)
          {
            double v = (double) img[y * w + x] + offset;
            img[y * w + x] = (guint8) CLAMP (v, 0.0, 255.0);
          }
    }

  guint32 hist[256] = {0};
  for (gsize i = 0; i < n; ++i)
    hist[img[i]]++;
  gsize lo_t = (n * 10) / 100, hi_t = (n * 90) / 100, cum = 0;
  guint8 lo = 0, hi = 255;
  for (int v = 0; v < 256; ++v)
    {
      cum += hist[v];
      if (cum >= lo_t)
        {
          lo = (guint8) v;
          break;
        }
    }
  cum = 0;
  for (int v = 0; v < 256; ++v)
    {
      cum += hist[v];
      if (cum >= hi_t)
        {
          hi = (guint8) v;
          break;
        }
    }
  if (hi > lo)
    {
      for (gsize i = 0; i < n; ++i)
        {
          int v = (((int) img[i] - lo) * 255) / (hi - lo);
          img[i] = (guint8) CLAMP (v, 0, 255);
        }
    }
}

/* mean absolute difference between two OUT buffers (inter-frame motion) */
guint
goodix_image_swipe_out_diff (const guint8 *a, const guint8 *b)
{
  guint64 sum = 0;
  const gsize n = (gsize) GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT;

  for (gsize i = 0; i < n; ++i)
    sum += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
  return (guint) (sum / n);
}

/* Local contrast normalisation of one swipe stripe (SWIPE_FRAME_W x _H).
 *
 * The sensor's response varies strongly with position: in captures from a
 * 55a2 the middle of a frame is about half as bright as its edges and one end
 * is close to saturation. That shading is the same in every frame, so without
 * this step motion estimation locks onto it and NBIS finds the same structure
 * in every finger (measured: other fingers scoring 35-45 against an enrolled
 * one). Subtract the local mean and divide by the local standard deviation
 * over a window of about two ridge periods. */
#define NORMALIZE_RADIUS 8
#define NORMALIZE_GAIN 48.0
#define NORMALIZE_MIN_STDDEV 4.0 /* keeps flat areas flat instead of amplifying noise */

void
goodix_image_normalize_stripe (guint8 *stripe)
{
  const gint w = GOODIX55X4_SWIPE_FRAME_W, h = GOODIX55X4_SWIPE_FRAME_H;
  const gint stride = w + 1;
  g_autofree gint64 *sum = g_new0 (gint64, stride * (h + 1));
  g_autofree gint64 *squares = g_new0 (gint64, stride * (h + 1));
  g_autofree guint8 *in = g_malloc (w * h);

  memcpy (in, stripe, w * h);

  /* Integral images: sum[y][x] covers rows < y and columns < x. */
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        gint64 v = in[y * w + x];
        gint at = (y + 1) * stride + x + 1;
        sum[at] = v + sum[at - stride] + sum[at - 1] - sum[at - stride - 1];
        squares[at] = v * v + squares[at - stride] + squares[at - 1] - squares[at - stride - 1];
      }

  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      {
        gint x0 = MAX (0, x - NORMALIZE_RADIUS), x1 = MIN (w, x + NORMALIZE_RADIUS + 1);
        gint y0 = MAX (0, y - NORMALIZE_RADIUS), y1 = MIN (h, y + NORMALIZE_RADIUS + 1);
        double n = (x1 - x0) * (y1 - y0);
        double s = sum[y1 * stride + x1] - sum[y0 * stride + x1] -
                   sum[y1 * stride + x0] + sum[y0 * stride + x0];
        double q = squares[y1 * stride + x1] - squares[y0 * stride + x1] -
                   squares[y1 * stride + x0] + squares[y0 * stride + x0];
        double mean = s / n;
        double stddev = sqrt (MAX (q / n - mean * mean, 0.0));
        double z = (in[y * w + x] - mean) / (stddev + NORMALIZE_MIN_STDDEV);

        stripe[y * w + x] = (guint8) CLAMP (128.0 + NORMALIZE_GAIN * z, 0.0, 255.0);
      }
}
