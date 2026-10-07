/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * Split out of goodix55x4.c. */
#pragma once
#include <glib.h>
/* 55a2 with the Windows config: raw image is 56 wide x 176 tall = 9856 px,
 * raw frame = 176*56/4*6 = 14784 bytes (matches the working Python capture_tl).
 * There are no padding columns in the decoded frame. */
#define GOODIX55X4_WIDTH 56
#define GOODIX55X4_HEIGHT 176
#define GOODIX55X4_FRAME_SIZE (GOODIX55X4_WIDTH * GOODIX55X4_HEIGHT)
/* Four 12-bit pixels are packed into six bytes. */
#define GOODIX55X4_RAW_FRAME_SIZE (GOODIX55X4_FRAME_SIZE / 4 * 6)

#define GOODIX55X4_CROP 4 /* drop 4 bogus rows/cols on every edge */
#define GOODIX55X4_OUT_WIDTH (GOODIX55X4_WIDTH - 2 * GOODIX55X4_CROP)   /* 48 */
#define GOODIX55X4_OUT_HEIGHT (GOODIX55X4_HEIGHT - 2 * GOODIX55X4_CROP) /* 168 */
/* rotated stripe: sensor long axis (168) -> width, short axis (48) -> height */
#define GOODIX55X4_SWIPE_FRAME_W GOODIX55X4_OUT_HEIGHT /* 168 */
#define GOODIX55X4_SWIPE_FRAME_H GOODIX55X4_OUT_WIDTH  /* 48  */

typedef guint16 Goodix55X4Pix;
void goodix_image_decode_frame (Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE], const guint8 *raw);
void goodix_image_postprocess_frame (Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE], const Goodix55X4Pix background[GOODIX55X4_FRAME_SIZE]);
gint goodix_image_swipe_raw_mean (const Goodix55X4Pix *frame);
void goodix_image_swipe_build_out (const Goodix55X4Pix *frame, guint8 *out);
void goodix_image_swipe_fpn_stretch (guint8 *image);
guint goodix_image_swipe_out_diff (const guint8 *a, const guint8 *b);
