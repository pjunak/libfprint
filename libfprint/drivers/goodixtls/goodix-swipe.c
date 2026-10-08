/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * Swipe processing split out of goodix55x4.c. */
#include "goodix-swipe.h"
#include <string.h>

#define FINGER_DROP 350
#define MOTION_THRESHOLD 12
#define KEEP_STEP 22

void
goodix_swipe_clear (GoodixSwipe *swipe)
{
  g_slist_free_full (swipe->stripes, g_free);
  memset (swipe, 0, sizeof (*swipe));
}

void
goodix_swipe_start (GoodixSwipe *swipe, gint baseline)
{
  goodix_swipe_clear (swipe);
  swipe->baseline = baseline;
}

gboolean
goodix_swipe_finger_present (GoodixSwipe *swipe, const Goodix55X4Pix *frame)
{
  return goodix_image_swipe_raw_mean (frame) < swipe->baseline - FINGER_DROP;
}

static struct fpi_frame *
make_stripe (const guint8 *out)
{
  const gsize pixels = GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT;
  struct fpi_frame *stripe = g_malloc0 (sizeof (*stripe) + pixels);
  guint hist[256] = {0};
  guint low = 0, high = 255, cumulative = 0;

  for (gsize i = 0; i < pixels; i++)
    hist[out[i]]++;
  for (guint value = 0; value < 256; value++)
    {
      cumulative += hist[value];
      if (cumulative >= pixels * 5 / 100)
        {
          low = value;
          break;
        }
    }
  cumulative = 0;
  for (guint value = 0; value < 256; value++)
    {
      cumulative += hist[value];
      if (cumulative >= pixels * 95 / 100)
        {
          high = value;
          break;
        }
    }
  high = MAX (high, low + 1);

  /* The sensor's long axis becomes the stripe width; the finger moves along
   * the short axis. */
  for (guint x = 0; x < GOODIX55X4_SWIPE_FRAME_W; x++)
    for (guint y = 0; y < GOODIX55X4_SWIPE_FRAME_H; y++)
      {
        gint value = ((gint) out[x * GOODIX55X4_OUT_WIDTH + y] - (gint) low) * 255 /
                     (gint) (high - low);
        stripe->data[x + y * GOODIX55X4_SWIPE_FRAME_W] = CLAMP (value, 0, 255);
      }
  goodix_image_normalize_stripe (stripe->data);
  return stripe;
}

static GoodixSwipeResult
swipe_result (GoodixSwipe *swipe)
{
  return swipe->n_stripes >= GOODIX_SWIPE_MIN_STRIPES ?
         GOODIX_SWIPE_COMPLETE : GOODIX_SWIPE_TOO_SHORT;
}

GoodixSwipeResult
goodix_swipe_feed (GoodixSwipe *swipe, Goodix55X4Pix *frame,
                   const Goodix55X4Pix *background)
{
  gint mean = goodix_image_swipe_raw_mean (frame);
  gboolean present = mean < swipe->baseline - FINGER_DROP;
  guint8 out[GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT];

  if (!present)
    {
      if (swipe->present)
        {
          swipe->present = FALSE;
          return swipe_result (swipe);
        }
      /* Follow the empty sensor's upper envelope without chasing a slowly
       * placed finger downwards. Idle frames never consume the swipe budget. */
      if (mean > swipe->baseline)
        swipe->baseline = (swipe->baseline * 7 + mean) / 8;
      return GOODIX_SWIPE_WAIT;
    }

  if (++swipe->n_frames > GOODIX_SWIPE_MAX_FRAMES)
    return GOODIX_SWIPE_REMOVE_FINGER;

  goodix_image_postprocess_frame (frame, background);
  goodix_image_swipe_build_out (frame, out);
  goodix_image_swipe_fpn_stretch (out);
  if (!swipe->present)
    {
      swipe->present = TRUE;
      memcpy (swipe->previous, out, sizeof (out));
      return GOODIX_SWIPE_FINGER_ON; /* discard the touch/settling frame */
    }

  guint difference = goodix_image_swipe_out_diff (out, swipe->previous);
  memcpy (swipe->previous, out, sizeof (out));
  if (difference <= MOTION_THRESHOLD)
    {
      if (++swipe->static_frames >= GOODIX_SWIPE_STATIC_FRAMES && swipe->moving)
        return swipe_result (swipe);
      return GOODIX_SWIPE_CONTINUE;
    }

  swipe->moving = TRUE;
  swipe->static_frames = 0;
  if (!swipe->have_kept || goodix_image_swipe_out_diff (out, swipe->kept) >= KEEP_STEP)
    {
      swipe->stripes = g_slist_prepend (swipe->stripes, make_stripe (out));
      swipe->n_stripes++;
      memcpy (swipe->kept, out, sizeof (out));
      swipe->have_kept = TRUE;
    }
  return swipe->n_stripes == GOODIX_SWIPE_MAX_STRIPES ?
         GOODIX_SWIPE_COMPLETE : GOODIX_SWIPE_CONTINUE;
}

static unsigned char
get_pixel (struct fpi_frame_asmbl_ctx *ctx, struct fpi_frame *frame,
           unsigned int x, unsigned int y)
{
  return frame->data[x + y * ctx->frame_width];
}

typedef struct
{
  gint    dx, dy;
  guint64 error;
} Shift;

/* Mean absolute difference (x256) between @cur and @prev displaced by
 * (dx, dy), i.e. cur(x, y) against prev(x + dx, y + dy), over the overlap. */
static guint64
shift_error (const guint8 *prev, const guint8 *cur, gint dx, gint dy)
{
  const gint w = GOODIX55X4_SWIPE_FRAME_W, h = GOODIX55X4_SWIPE_FRAME_H;
  const gint x0 = MAX (0, -dx), x1 = MIN (w, w - dx);
  const gint y0 = MAX (0, -dy), y1 = MIN (h, h - dy);
  guint64 sum = 0;

  for (gint y = y0; y < y1; y++)
    {
      const guint8 *c = cur + y * w;
      const guint8 *p = prev + (y + dy) * w + dx;
      for (gint x = x0; x < x1; x++)
        sum += ABS ((gint) c[x] - (gint) p[x]);
    }
  return sum * 256 / ((guint64) (x1 - x0) * (y1 - y0));
}

/* Best displacement of @cur relative to @prev for each swipe direction.
 * Ties go to the smallest movement (featureless stripes stay in place). */
static void
best_shifts (const guint8 *prev, const guint8 *cur, Shift *down, Shift *up)
{
  down->error = up->error = G_MAXUINT64;
  for (gint dy = 1; dy <= GOODIX_SWIPE_MAX_SHIFT_Y; dy++)
    for (gint k = 0; k <= 2 * GOODIX_SWIPE_MAX_SHIFT_X; k++)
      {
        const gint dx = k % 2 ? -(k + 1) / 2 : k / 2; /* 0, -1, 1, -2, 2, ... */
        guint64 error = shift_error (prev, cur, dx, dy);
        if (error < down->error)
          *down = (Shift){ dx, dy, error };
        error = shift_error (prev, cur, dx, -dy);
        if (error < up->error)
          *up = (Shift){ dx, -dy, error };
      }
}

/* Set each stripe's delta_x/delta_y from the overlap with its predecessor.
 * Kept stripes overlap heavily (the finger moves a few rows per frame), so
 * placing them edge to edge would repeat each ridge several times. A swipe
 * goes one way, so pick the direction that fits the whole swipe best. */
static void
estimate_motion (GSList *stripes)
{
  guint n = g_slist_length (stripes);
  g_autofree Shift *down = g_new (Shift, n);
  g_autofree Shift *up = g_new (Shift, n);
  guint64 down_total = 0, up_total = 0;
  struct fpi_frame *prev = stripes->data;
  guint i = 1;

  for (GSList *item = stripes->next; item; item = item->next, i++)
    {
      struct fpi_frame *cur = item->data;
      best_shifts (prev->data, cur->data, &down[i], &up[i]);
      down_total += down[i].error;
      up_total += up[i].error;
      prev = cur;
    }

  Shift *chosen = down_total <= up_total ? down : up;
  i = 1;
  for (GSList *item = stripes->next; item; item = item->next, i++)
    {
      struct fpi_frame *frame = item->data;
      frame->delta_x = chosen[i].dx;
      frame->delta_y = chosen[i].dy;
    }
}

FpImage *
goodix_swipe_assemble (GSList *stripes)
{
  struct fpi_frame_asmbl_ctx ctx = {
    .frame_width = GOODIX55X4_SWIPE_FRAME_W,
    .frame_height = GOODIX55X4_SWIPE_FRAME_H,
    .image_width = GOODIX_SWIPE_IMAGE_W,
    .get_pixel = get_pixel,
  };

  g_return_val_if_fail (stripes && stripes->next, NULL);
  estimate_motion (stripes);
  FpImage *image = fpi_assemble_frames (&ctx, stripes);
  image->ppmm = 500.0 / 25.4;
  image->flags |= FPI_IMAGE_COLORS_INVERTED;
  return image;
}

FpImage *
goodix_swipe_take_image (GoodixSwipe *swipe)
{
  g_return_val_if_fail (swipe->n_stripes >= GOODIX_SWIPE_MIN_STRIPES, NULL);
  swipe->stripes = g_slist_reverse (swipe->stripes);
  FpImage *image = goodix_swipe_assemble (swipe->stripes);
  g_slist_free_full (g_steal_pointer (&swipe->stripes), g_free);
  swipe->n_stripes = 0;
  return image;
}
