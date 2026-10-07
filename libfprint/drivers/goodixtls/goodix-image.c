/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * Split out of goodix55x4.c. */
#include "goodix-image.h"

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
