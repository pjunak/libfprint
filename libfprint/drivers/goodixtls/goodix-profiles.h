/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>
 *
 * Split out of goodix55x4.c. */
#pragma once
#include <glib.h>
typedef struct {
  guint16 pid;
  const gchar *firmware;
  guint width, height, crop;
  guint reset_number, tls_settle_ms;
  const guint8 *config;
  gsize config_length;
} GoodixProfile;
/* NULL means unsupported: never guess geometry or upload a different model's config. */
const GoodixProfile *goodix_profile_lookup (guint16 pid, const gchar *firmware);
