/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include <gio/gio.h>

gboolean capture_write_pgm (const gchar *path, guint width, guint height,
                            const guint8 *pixels, gsize size, GError **error);
