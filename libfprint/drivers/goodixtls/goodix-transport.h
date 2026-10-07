/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 *
 * Split out of goodix.c. */
#pragma once
#include "drivers_api.h"

typedef void (*GoodixWriteDone) (FpDevice *device, gpointer data, GError *error);
/* References bytes and cancellable until completion; callback owns the error.
 * Keeps firmware-required 64-byte transfers, with one total write deadline. */
void goodix_write_async (FpDevice *device, guint8 endpoint, GBytes *bytes,
                         GCancellable *cancellable, GoodixWriteDone callback,
                         gpointer user_data);
