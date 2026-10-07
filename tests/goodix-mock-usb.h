/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once
#include "drivers_api.h"

static GByteArray *sent_data;
typedef struct {
  FpiUsbTransfer *transfer;
  GCancellable *cancellable;
  FpiUsbTransferCallback callback;
  gpointer data;
  gulong cancel_handler;
} MockTransfer;
static gboolean fail_write;
static MockTransfer *pending_read;
static GQueue incoming = G_QUEUE_INIT;
static guint read_idle;
static void (*observe_write) (FpDevice *, const guint8 *, gsize);

static void
free_mock_transfer (MockTransfer *mock)
{
  if (mock->cancel_handler)
    g_signal_handler_disconnect (mock->cancellable, mock->cancel_handler);
  fpi_usb_transfer_unref (mock->transfer);
  g_object_unref (mock->cancellable);
  g_free (mock);
}

static gboolean
complete_mock_read (gpointer unused)
{
  MockTransfer *mock = g_steal_pointer (&pending_read);
  GError *error = NULL;
  read_idle = 0;
  if (g_cancellable_is_cancelled (mock->cancellable))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled mock read");
  else
    {
      g_autoptr(GBytes) bytes = g_queue_pop_head (&incoming);
      gsize size;
      const guint8 *data = g_bytes_get_data (bytes, &size);
      g_assert_cmpuint (size, <=, mock->transfer->length);
      memcpy (mock->transfer->buffer, data, size);
      mock->transfer->actual_length = size;
    }
  mock->callback (mock->transfer, mock->transfer->device, mock->data, error);
  free_mock_transfer (mock);
  return G_SOURCE_REMOVE;
}

static void
schedule_mock_read (GCancellable *unused, gpointer data)
{
  if (pending_read && !read_idle &&
      (!g_queue_is_empty (&incoming) || g_cancellable_is_cancelled (pending_read->cancellable)))
    read_idle = g_idle_add (complete_mock_read, NULL);
}

static gboolean
complete_mock_transfer (gpointer data)
{
  MockTransfer *mock = data;
  GError *error = NULL;
  if (g_cancellable_is_cancelled (mock->cancellable))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled mock USB transfer");
  else if (fail_write) {
    fail_write = FALSE;
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "mock write timeout");
  } else {
    g_assert_nonnull (sent_data);
    g_byte_array_append (sent_data, mock->transfer->buffer, mock->transfer->length);
    mock->transfer->actual_length = mock->transfer->length;
    if (observe_write)
      observe_write (mock->transfer->device, mock->transfer->buffer, mock->transfer->length);
  }
  mock->callback (mock->transfer, mock->transfer->device, mock->data, error);
  free_mock_transfer (mock);
  return G_SOURCE_REMOVE;
}

void
record_usb_submit (FpiUsbTransfer *transfer, guint timeout, GCancellable *cancellable,
                   FpiUsbTransferCallback callback, gpointer user_data)
{
  MockTransfer *mock = g_new0 (MockTransfer, 1);
  *mock = (MockTransfer) {transfer, g_object_ref (cancellable), callback, user_data, 0};
  if (transfer->endpoint & 0x80)
    {
      g_assert_null (pending_read);
      pending_read = mock;
      mock->cancel_handler = g_signal_connect (cancellable, "cancelled", G_CALLBACK (schedule_mock_read), NULL);
      schedule_mock_read (NULL, NULL);
    }
  else
    {
      g_assert_cmpuint (timeout, >, 0);
      g_idle_add (complete_mock_transfer, mock);
    }
}

static void
drain_writes (void)
{
  while (g_main_context_iteration (NULL, FALSE));
}

