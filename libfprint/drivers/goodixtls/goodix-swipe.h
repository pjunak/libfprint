/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * Split out of goodix55x4.c. */
#pragma once

#include "drivers_api.h"
#include "goodix-image.h"

/* Preserve the fork's recognition parameters until a representative capture
 * corpus supports changing them. Frame limits apply to a touched sensor only. */
#define GOODIX_SWIPE_MIN_STRIPES 12
#define GOODIX_SWIPE_MAX_STRIPES 60
#define GOODIX_SWIPE_MAX_FRAMES 700
#define GOODIX_SWIPE_STATIC_FRAMES 4

typedef enum {
  GOODIX_SWIPE_WAIT,
  GOODIX_SWIPE_FINGER_ON,
  GOODIX_SWIPE_CONTINUE,
  GOODIX_SWIPE_COMPLETE,
  GOODIX_SWIPE_TOO_SHORT,
  GOODIX_SWIPE_REMOVE_FINGER,
} GoodixSwipeResult;

/* No USB, timers, environment variables or libfprint completion callbacks.
 * The driver owns the calibration and decides when the user has lifted. */
typedef struct {
  GSList *stripes;
  guint n_stripes, n_frames, static_frames;
  gboolean present, moving, have_kept;
  gint baseline;
  guint8 previous[GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT];
  guint8 kept[GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT];
} GoodixSwipe;

void goodix_swipe_clear (GoodixSwipe *swipe);
void goodix_swipe_start (GoodixSwipe *swipe, gint baseline);
gboolean goodix_swipe_finger_present (GoodixSwipe *swipe, const Goodix55X4Pix *frame);
GoodixSwipeResult goodix_swipe_feed (GoodixSwipe *swipe, Goodix55X4Pix *frame,
                                   const Goodix55X4Pix *background);
FpImage *goodix_swipe_take_image (GoodixSwipe *swipe);
