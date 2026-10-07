/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 *
 * Split out of goodix.c. */
#include "goodix-transport.h"
#include "goodix_proto.h"

typedef struct
{
  FpDevice       *device;
  GBytes         *bytes;
  GCancellable   *cancellable;
  guint8          endpoint;
  gsize           offset;
  gint64          deadline;
  GoodixWriteDone callback;
  gpointer        user_data;
} Write;

static void write_next (Write *write);

static void
write_finish (Write *write, GError *error)
{
  write->callback (write->device, write->user_data, error);
  g_object_unref (write->device);
  g_bytes_unref (write->bytes);
  g_object_unref (write->cancellable);
  g_free (write);
}

static void
write_done (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  Write *write = user_data;

  if (error)
    {
      write_finish (write, error);
    }
  else if (g_cancellable_is_cancelled (write->cancellable))
    {
      write_finish (write, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "USB write cancelled"));
    }
  else
    {
      write->offset += transfer->actual_length;
      write_next (write);
    }
}

static void
write_next (Write *write)
{
  gsize length;
  const guint8 *bytes = g_bytes_get_data (write->bytes, &length);
  gint64 remaining = write->deadline - g_get_monotonic_time ();

  if (write->offset == length)
    {
      write_finish (write, NULL);
      return;
    }
  if (remaining <= 0)
    {
      write_finish (write, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "USB write deadline exceeded"));
      return;
    }
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (write->device);
  gsize chunk = MIN (GOODIX_EP_OUT_MAX_BUF_SIZE, length - write->offset);

  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (transfer, write->endpoint,
                                   (guint8 *) bytes + write->offset, chunk, NULL);
  fpi_usb_transfer_submit (transfer, MAX (1, (remaining + 999) / 1000), write->cancellable,
                           write_done, write);
}

void
goodix_write_async (FpDevice *device, guint8 endpoint, GBytes *bytes,
                    GCancellable *cancellable, GoodixWriteDone callback, gpointer user_data)
{
  Write *write = g_new0 (Write, 1);

  write->device = g_object_ref (device);
  write->bytes = g_bytes_ref (bytes);
  write->cancellable = g_object_ref (cancellable);
  write->endpoint = endpoint;
  write->deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
  write->callback = callback;
  write->user_data = user_data;
  write_next (write);
}
