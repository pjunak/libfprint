// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#define FP_COMPONENT "goodixtls"

#include "fpi-byte-utils.h"

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodixtls.h"
#include "goodix-transport.h"

typedef struct
{
  GoodixTlsServer    *tls_hop;

  GSource            *timeout;

  guint8              cmd;
  guint8              response_flags;
  guint               reply_timeout_ms;
  guint               generation;
  gboolean            write_only;
  GCancellable       *write_cancel;
  guint               tls_settle_ms;

  gboolean            ack;
  gboolean            reply;

  GoodixCmdCallback   callback;
  gpointer            user_data;

  GByteArray         *receive_buffer;

  GoodixCallbackInfo *tls_ready_callback;
  FpiSsm             *tls_ssm;
  GSource            *tls_settle_source;

  GCancellable       *transfer_cancel_tkn;
  gboolean            inited;
  gboolean            read_pending;
} FpiDeviceGoodixTlsPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (FpiDeviceGoodixTls, fpi_device_goodixtls,
                                     FP_TYPE_IMAGE_DEVICE);

// ---- GOODIX RECEIVE SECTION START ----

void
goodix_receive_done (FpDevice *dev, guint8 *data, guint16 length,
                     GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GoodixCmdCallback callback = priv->callback;
  gpointer user_data = priv->user_data;

  if (!(priv->ack || priv->reply))
    {
      g_clear_error (&error);
      return;
    }

  goodix_reset_state (dev);
  if (!error)
    fp_dbg ("Completed command: 0x%02x", priv->cmd);

  if (callback)
    callback (dev, data, length, user_data, error);
  else
    g_clear_error (&error);
}

void
goodix_receive_none (FpDevice *dev, guint8 *data, guint16 length,
                     gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixNoneCallback callback = (GoodixNoneCallback) cb_info->callback;

  callback (dev, cb_info->user_data, error);
}

void
goodix_receive_default (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixDefaultCallback callback = (GoodixDefaultCallback) cb_info->callback;

  callback (dev, data, length, cb_info->user_data, error);
}

void
goodix_receive_success (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixSuccessCallback callback = (GoodixSuccessCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  if (length != sizeof (guint8) * 2)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid success reply length: %d", length);
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  callback (dev, data[0] == 0x00 ? FALSE : TRUE, cb_info->user_data, NULL);
}

void
goodix_receive_reset (FpDevice *dev, guint8 *data, guint16 length,
                      gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixResetCallback callback = (GoodixResetCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, 0, cb_info->user_data, error);
      return;
    }

  if (length != sizeof (guint8) + sizeof (guint16))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid reset reply length: %d", length);
      callback (dev, FALSE, 0, cb_info->user_data, error);
      return;
    }

  callback (dev, data[0] == 0x00 ? FALSE : TRUE,
            FP_READ_UINT16_LE (data + 1),
            cb_info->user_data, NULL);
}

void
goodix_receive_preset_psk_read (FpDevice *dev, guint8 *data, guint16 length,
                                gpointer user_data, GError *error)
{
  guint32 psk_len;
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixPresetPskReadCallback callback =
    (GoodixPresetPskReadCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;
    }

  if (length < sizeof (guint8))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid preset PSK read reply length: %d", length);
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;
    }

  if (data[0] != 0x00)
    {
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, NULL);
      return;
    }

  /* Reply: status byte, little-endian flags and length, then PSK hash.
   * Unlike the request, the reply does not contain an offset field. */
  const guint header_len = 1 + 2 * sizeof (guint32);
  if (length < header_len)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Truncated preset PSK reply: %u bytes", length);
      callback (dev, FALSE, 0, NULL, 0, cb_info->user_data, error);
      return;
    }

  psk_len = FP_READ_UINT32_LE (data + 5);
  if (psk_len != length - header_len)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Preset PSK length %u does not match %u payload bytes",
                   psk_len, length - header_len);
      callback (dev, FALSE, 0, NULL, 0, cb_info->user_data, error);
      return;
    }

  callback (dev, TRUE, FP_READ_UINT32_LE (data + 1),
            data + header_len, psk_len, cb_info->user_data, NULL);
}

void
goodix_receive_firmware_version (FpDevice *dev, guint8 *data,
                                 guint16 length, gpointer user_data,
                                 GError *error)
{
  g_autofree gchar *payload = g_malloc (length + sizeof (gchar));
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixFirmwareVersionCallback callback =
    (GoodixFirmwareVersionCallback) cb_info->callback;

  if (error)
    {
      callback (dev, NULL, cb_info->user_data, error);
      return;
    }

  memcpy (payload, data, length);

  // Some device send the firmware without the null terminator
  payload[length] = 0x00;

  callback (dev, payload, cb_info->user_data, NULL);
}

void
goodix_receive_ack (FpDevice *dev, guint8 *data, guint16 length,
                    gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GoodixAck *ack = (GoodixAck *) data;

  if (length != sizeof (GoodixAck))
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "Invalid ACK length: %u", length));
      return;
    }

  if (priv->cmd != ack->cmd || !priv->ack)
    {
      fp_dbg ("Ignoring stale ACK for command 0x%02x", ack->cmd);
      return;
    }

  if (!ack->always_true)
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "Invalid ACK flags for command 0x%02x: 0x%02x",
                                        ack->cmd, data[1]));
      return;
    }

  if (ack->has_no_config)
    fp_warn ("MCU has no config");

  if (!priv->reply)
    {
      G_DEBUG_HERE ();
      goodix_receive_done (dev, NULL, 0, NULL);
      return;
    }

  priv->ack = FALSE;
  g_clear_pointer (&priv->timeout, g_source_destroy);
  if (priv->reply_timeout_ms)
    priv->timeout = fpi_device_add_timeout (dev, priv->reply_timeout_ms,
                                            goodix_receive_timeout_cb, NULL, NULL);
}

void
goodix_receive_protocol (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  guint8 cmd;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum, valid_null_checksum;

  if (!goodix_decode_protocol (data, length, &cmd, &payload, &payload_len,
                               &valid_checksum, &valid_null_checksum))
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                "Truncated Goodix protocol message"));
      return;
    }

  if (!valid_checksum && !valid_null_checksum)
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                "Invalid Goodix protocol checksum"));
      return;
    }

  if (cmd == GOODIX_CMD_ACK)
    {
      fp_dbg ("got ack");
      goodix_receive_ack (dev, payload, payload_len, NULL, NULL);
      return;
    }

  /* Firmware can ask for a new TLS session instead of supplying an image.
   * Surface this immediately, rather than waiting for an unrelated timeout. */
  if (cmd == GOODIX_CMD_REQUEST_TLS_CONNECTION && priv->reply &&
      priv->cmd == GOODIX_CMD_MCU_GET_IMAGE)
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                                                "Sensor lost its TLS session during capture"));
      return;
    }

  if (priv->cmd != cmd)
    {
      fp_dbg ("Ignoring unsolicited protocol reply 0x%02x (waiting for 0x%02x)", cmd, priv->cmd);
      return;
    }

  if (!priv->reply || priv->write_only)
    {
      fp_dbg ("Ignoring unsolicited reply for command: 0x%02x", priv->cmd);
      return;
    }

  if (priv->response_flags != GOODIX_FLAGS_MSG_PROTOCOL)
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "Expected encrypted reply to command 0x%02x", cmd));
      return;
    }

  if (priv->ack)
    fp_warn ("Didn't get ACK for command: 0x%02x", priv->cmd);

  goodix_receive_done (dev, payload, payload_len, NULL);
}

void
goodix_receive_pack (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  g_autoptr(GByteArray) buffer = g_steal_pointer (&priv->receive_buffer);
  g_autoptr(GCancellable) cancellable = priv->transfer_cancel_tkn ?
                                        g_object_ref (priv->transfer_cancel_tkn) : NULL;
  guint offset = 0;

  if (!buffer)
    buffer = g_byte_array_new ();
  /* One unfinished 16-bit packet plus one USB transfer is the maximum input
   * needed. GByteArray grows geometrically instead of reallocating every byte. */
  if (length > GOODIX_EP_IN_MAX_BUF_SIZE || buffer->len > G_MAXUINT16 + 4)
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                "Oversized Goodix receive buffer"));
      return;
    }
  g_byte_array_append (buffer, data, length);

  while (offset < buffer->len)
    {
      GoodixPacket packet;
      gint result = goodix_packet_peek (buffer->data + offset, buffer->len - offset, &packet);
      offset += packet.consumed;
      if (result == 0)
        break;
      if (result < 0)
        {
          goodix_receive_done (dev, NULL, 0,
                               g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                    "Invalid Goodix packet checksum"));
          return;
        }
      guint8 flags = packet.flags;
      guint8 *payload = (guint8 *) packet.payload;
      guint16 payload_len = packet.length;

      switch (flags)
        {
        case GOODIX_FLAGS_MSG_PROTOCOL:
          goodix_receive_protocol (dev, payload, payload_len);
          break;

        case GOODIX_FLAGS_TLS:
          if (priv->reply && !priv->write_only && priv->response_flags == flags)
            goodix_receive_done (dev, payload, payload_len, NULL);
          else
            fp_dbg ("Ignoring TLS handshake packet outside a handshake read");
          break;

        case GOODIX_FLAGS_TLS_DATA:
          if (!priv->reply || priv->write_only || priv->response_flags != flags)
            {
              fp_dbg ("Ignoring image packet outside an image read");
              break;
            }
          /* 55x4 image records have a nine-byte prefix before TLS content. */
          if (payload_len < 9)
            goodix_receive_done (dev, NULL, 0,
                                 g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                      "Truncated Goodix TLS image header"));
          else
            goodix_receive_done (dev, payload + 9, payload_len - 9, NULL);
          break;

        default:
          goodix_receive_done (dev, NULL, 0,
                               g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                            "Unknown Goodix packet flags: 0x%02x", flags));
          return;
        }

      /* A completion may cancel the scan and start a new session. Remaining
       * bytes from this transfer still belong to the old session. */
      if (cancellable && (g_cancellable_is_cancelled (cancellable) ||
                          cancellable != priv->transfer_cancel_tkn))
        return;
    }

  if (offset < buffer->len)
    {
      g_byte_array_remove_range (buffer, 0, offset);
      priv->receive_buffer = g_steal_pointer (&buffer);
    }
}

void
goodix_receive_data_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                        gpointer user_data, GError *error)
{
  g_autoptr(GCancellable) cancellable = user_data;
  /* An action can finish before libusb acknowledges cancellation. Keep the
   * device alive until the last callback, including close/reopen races. */
  g_autoptr(FpDevice) device_ref = dev;
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->read_pending = FALSE;
  if (g_cancellable_is_cancelled (cancellable) || cancellable != priv->transfer_cancel_tkn)
    {
      fp_dbg ("transfer cancelled, aborting read loop...");
      g_clear_error (&error);
      if (priv->inited)
        goodix_receive_data (dev);
      return;
    }
  if (error)
    {
      priv->inited = FALSE;
      goodix_receive_done (dev, NULL, 0, error);
      return;
    }

  goodix_receive_pack (dev, transfer->buffer, transfer->actual_length);

  if (priv->inited && priv->transfer_cancel_tkn == cancellable)
    goodix_receive_data (dev);
}

void
goodix_receive_timeout_cb (FpDevice *dev, gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GError *error = NULL;

  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
               "Goodix command 0x%02x timed out waiting for %s", priv->cmd,
               priv->ack ? "ACK" : "reply");
  goodix_receive_done (dev, NULL, 0, error);
}

void
goodix_start_read_loop (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->inited)
    // Already going
    return;
  else
    priv->inited = TRUE;
  /* Never reset a token still owned by a cancelled USB transfer. Its callback
   * may arrive after a new activation has already started. */
  if (!priv->transfer_cancel_tkn ||
      g_cancellable_is_cancelled (priv->transfer_cancel_tkn))
    {
      g_clear_object (&priv->transfer_cancel_tkn);
      priv->transfer_cancel_tkn = g_cancellable_new ();
    }

  goodix_receive_data (dev);
}

void
goodix_receive_data (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  /* A new session can be requested before the cancelled read calls back.
   * That callback starts its replacement; never submit two IN reads. */
  if (priv->read_pending || !priv->inited)
    return;

  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  priv->read_pending = TRUE;
  g_object_ref (dev);

  transfer->short_is_error = FALSE;

  fpi_usb_transfer_fill_bulk (transfer, class->ep_in,
                              GOODIX_EP_IN_MAX_BUF_SIZE);

  fpi_usb_transfer_submit (transfer, 0, priv->transfer_cancel_tkn,
                           goodix_receive_data_cb,
                           g_object_ref (priv->transfer_cancel_tkn));
}

// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

static void
command_write_done (FpDevice *dev, gpointer generation, GError *error)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  /* A cancellation/completion can start a new command before USB calls back. */
  if (priv->generation != GPOINTER_TO_UINT (generation))
    {
      g_clear_error (&error);
      return;
    }
  if (error || priv->write_only)
    goodix_receive_done (dev, NULL, 0, error);
  else if (!priv->timeout && priv->ack)
    priv->timeout = fpi_device_add_timeout (dev, GOODIX_TIMEOUT,
                                            goodix_receive_timeout_cb, NULL, NULL);
}

static void
goodix_send_pack (FpDevice *dev, guint8 flags, guint8 *payload, guint16 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv = fpi_device_goodixtls_get_instance_private (self);
  guint8 *data;
  guint32 size;

  goodix_encode_pack (flags, payload, length, TRUE, &data, &size);
  g_autoptr(GBytes) bytes = g_bytes_new_take (data, size);
  g_clear_object (&priv->write_cancel);
  priv->write_cancel = g_cancellable_new ();
  goodix_write_async (dev, FPI_DEVICE_GOODIXTLS_GET_CLASS (self)->ep_out,
                      bytes, priv->write_cancel, command_write_done,
                      GUINT_TO_POINTER (priv->generation));
}

void
goodix_send_protocol (
  FpDevice *dev, guint8 cmd, guint8 *payload, guint16 length,
  GDestroyNotify free_func, gboolean calc_checksum, guint timeout_ms,
  gboolean reply, GoodixCmdCallback callback, gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  g_autofree guint8 *data = NULL;
  guint32 data_len;

  if (priv->ack || priv->reply || priv->timeout)
    {
      // A command is already running.
      fp_warn ("A command is already running: 0x%02x", priv->cmd);
      if (free_func)
        free_func (payload);
      if (callback)
        callback (dev, NULL, 0, user_data,
                  g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PENDING,
                                       "A Goodix command is already running"));
      return;
    }

  fp_dbg ("Running command: 0x%02x", cmd);

  if (length > G_MAXUINT16 - sizeof (GoodixProtocol) - 1)
    {
      if (free_func)
        free_func (payload);
      if (callback)
        callback (dev, NULL, 0, user_data,
                  g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Goodix command payload is too large"));
      return;
    }
  priv->generation++;
  priv->cmd = cmd;
  priv->response_flags = cmd == GOODIX_CMD_MCU_GET_IMAGE ? GOODIX_FLAGS_TLS_DATA :
                         cmd == GOODIX_CMD_REQUEST_TLS_CONNECTION ? GOODIX_FLAGS_TLS :
                         GOODIX_FLAGS_MSG_PROTOCOL;
  priv->write_only = cmd == GOODIX_CMD_NOP;
  priv->ack = !priv->write_only;
  priv->reply = reply || priv->write_only;
  priv->reply_timeout_ms = timeout_ms;
  priv->callback = callback;
  priv->user_data = user_data;

  goodix_encode_protocol (cmd, payload, length, calc_checksum, FALSE,
                          &data, &data_len);
  if (free_func)
    free_func (payload);

  goodix_send_pack (dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len);
}

/* Typed response adapters share one ownership path. A wrapper is allocated
 * only when a caller supplied a callback, and the decoder always frees it. */
static void
send_command (FpDevice *dev, guint8 command, guint8 *payload, guint16 length,
              GDestroyNotify destroy, gboolean checksum, guint timeout,
              gboolean reply, GoodixCmdCallback decoder, GCallback callback, gpointer user_data)
{
  GoodixCallbackInfo *info = NULL;

  if (callback)
    {
      info = g_new (GoodixCallbackInfo, 1);
      *info = (GoodixCallbackInfo){callback, user_data};
    }
  goodix_send_protocol (dev, command, payload, length, destroy, checksum, timeout,
                        reply, callback ? decoder : NULL, info);
}

void
goodix_send_nop (FpDevice *dev, GoodixNoneCallback callback,
                 gpointer user_data)
{
  GoodixNop payload = {.unknown = 0x00000000};

  send_command (dev, GOODIX_CMD_NOP, (guint8 *) &payload,
                sizeof (payload), NULL, FALSE, 0, FALSE,
                goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_get_image (FpDevice *dev, GoodixImageCallback callback,
                           gpointer user_data)
{
  GoodixDefault payload = {.unused_flags = 0x01};

  send_command (dev, GOODIX_CMD_MCU_GET_IMAGE, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_default, G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_fdt_down (FpDevice *dev, guint8 *mode,
                                    guint16 length,
                                    GDestroyNotify free_func,
                                    GoodixDefaultCallback callback,
                                    gpointer user_data)
{
  send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode,
                length, free_func, TRUE, 0, TRUE,
                goodix_receive_default, G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_fdt_up (FpDevice *dev, guint8 *mode,
                                  guint16 length, GDestroyNotify free_func,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode,
                length, free_func, TRUE, 0, TRUE,
                goodix_receive_default, G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_fdt_mode (FpDevice *dev, guint8 *mode,
                                    guint16 length,
                                    GDestroyNotify free_func,
                                    GoodixDefaultCallback callback,
                                    gpointer user_data)
{
  send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode,
                length, free_func, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_default, G_CALLBACK (callback), user_data);
}

void
goodix_send_nav_0 (FpDevice *dev, GoodixDefaultCallback callback,
                   gpointer user_data)
{
  GoodixDefault payload = {.unused_flags = 0x01};

  /* 55a2 firmware only ACKs nav_0; it sends no separate data reply, so
   * wait for the ACK only (reply=FALSE) to avoid a timeout. */
  send_command (dev, GOODIX_CMD_NAV_0, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, 0, FALSE,
                goodix_receive_default, G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_idle_mode (FpDevice *dev, guint8 sleep_time,
                                     GoodixNoneCallback callback,
                                     gpointer user_data)
{
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};

  send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_set_led (FpDevice *dev, guint8 state, GoodixNoneCallback callback,
                     gpointer user_data)
{
  /* Retained capture-setup command. It has not been shown to control the
   * visible LED. Only its ACK is required; an extra data reply is ignored. */
  GoodixSetLed payload = {.state = state};

  send_command (dev, GOODIX_CMD_SET_LED, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_sleep_mode (FpDevice *dev, guint8 sleep_time,
                                      GoodixNoneCallback callback,
                                      gpointer user_data)
{
  GoodixMcuSwitchToIdleMode payload = {.sleep_time = sleep_time};

  send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_mcu_switch_to_sleep_mode_realtek (FpDevice *dev, guint8 value,
                                              GoodixSuccessCallback callback,
                                              gpointer user_data)
{
  GoodixMcuSwitchToSleepModeRealtek payload = {.value = value};

  send_command (dev, GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE_REALTEK, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_success, G_CALLBACK (callback), user_data);
}

void
goodix_send_write_sensor_register (FpDevice *dev, guint16 address,
                                   guint16 value,
                                   GoodixNoneCallback callback,
                                   gpointer user_data)
{
  // Only support one address and one value

  GoodixWriteSensorRegister payload = {
    .multiples = FALSE,
    .address = GUINT16_TO_LE (address),
    .value = GUINT16_TO_LE (value),
  };

  send_command (dev, GOODIX_CMD_WRITE_SENSOR_REGISTER, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_upload_config_mcu (FpDevice *dev, guint8 *config,
                               guint16 length, GDestroyNotify free_func,
                               GoodixSuccessCallback callback,
                               gpointer user_data)
{
  send_command (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config,
                length, free_func, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_success, G_CALLBACK (callback), user_data);
}

void
goodix_send_enable_chip (FpDevice *dev, gboolean enable,
                         GoodixNoneCallback callback, gpointer user_data)
{
  GoodixEnableChip payload = {.enable = enable ? TRUE : FALSE};

  send_command (dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_reset (FpDevice *dev, gboolean reset_sensor, guint8 sleep_time,
                   GoodixResetCallback callback, gpointer user_data)
{
  // Only support reset sensor

  GoodixReset payload = {.soft_reset_mcu = FALSE,
                         .reset_sensor = reset_sensor ? TRUE : FALSE,
                         .other = 1,
                         .sleep_time = sleep_time};

  send_command (dev, GOODIX_CMD_RESET, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_reset, G_CALLBACK (callback), user_data);
}

void
goodix_send_firmware_version (FpDevice                     *dev,
                              GoodixFirmwareVersionCallback callback,
                              gpointer                      user_data)
{
  GoodixNone payload = {0};

  send_command (dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_firmware_version, G_CALLBACK (callback), user_data);
}

void
goodix_send_query_mcu_state (FpDevice *dev, GoodixDefaultCallback callback,
                             gpointer user_data)
{
  GoodixQueryMcuState payload = {.unused_flags = 0x55};

  send_command (dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_default, G_CALLBACK (callback), user_data);
}

void
goodix_send_request_tls_connection (FpDevice             *dev,
                                    GoodixDefaultCallback callback,
                                    gpointer              user_data)
{
  GoodixNone payload = {0};

  send_command (dev, GOODIX_CMD_REQUEST_TLS_CONNECTION, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_default, G_CALLBACK (callback), user_data);
}

void
goodix_send_tls_successfully_established (FpDevice          *dev,
                                          GoodixNoneCallback callback,
                                          gpointer           user_data)
{
  GoodixNone payload = {0};

  send_command (dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                goodix_receive_none, G_CALLBACK (callback), user_data);
}

void
goodix_send_preset_psk_read (FpDevice *dev, guint32 flags, guint16 length,
                             GoodixPresetPskReadCallback callback,
                             gpointer user_data)
{
  GoodixPresetPsk payload = {.flags = GUINT32_TO_LE (flags),
                             .length = GUINT32_TO_LE (length)};

  send_command (dev, GOODIX_CMD_PRESET_PSK_READ, (guint8 *) &payload,
                sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                goodix_receive_preset_psk_read, G_CALLBACK (callback), user_data);
}

// ---- GOODIX SEND SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

gboolean
goodix_dev_init (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->timeout = NULL;
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;
  g_clear_pointer (&priv->receive_buffer, g_byte_array_unref);
  return g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                       class->interface, 0, error);
}
void
goodix_reset_state (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->timeout)
    g_clear_pointer (&priv->timeout, g_source_destroy);
  priv->generation++;
  if (priv->write_cancel)
    g_cancellable_cancel (priv->write_cancel);
  g_clear_object (&priv->write_cancel);
  priv->write_only = FALSE;
  priv->reply_timeout_ms = 0;
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;
}

void
goodix_cancel_receive (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->inited = FALSE;
  if (priv->transfer_cancel_tkn)
    g_cancellable_cancel (priv->transfer_cancel_tkn);
  g_clear_pointer (&priv->receive_buffer, g_byte_array_unref);
}

gboolean
goodix_read_pending (FpDevice *dev)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  return priv->read_pending;
}

gboolean
goodix_reset_usb (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  GUsbDevice *usb = fpi_device_get_usb_device (dev);

  g_autoptr(GError) local_error = NULL;

  g_return_val_if_fail (!priv->read_pending, FALSE);
  g_clear_pointer (&priv->receive_buffer, g_byte_array_unref);
  /* Releasing can fail on an already wedged device; the reset still applies. */
  if (!g_usb_device_release_interface (usb, class->interface, 0, &local_error))
    fp_dbg ("Releasing the interface before reset failed: %s", local_error->message);
  if (!g_usb_device_reset (usb, error))
    return FALSE;
  return g_usb_device_claim_interface (usb, class->interface, 0, error);
}

gboolean
goodix_dev_deinit (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  goodix_cancel_receive (dev);
  g_clear_object (&priv->transfer_cancel_tkn);
  goodix_shutdown_tls (dev, error);

  goodix_reset_state (dev);
  priv->inited = FALSE;

  return g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                         class->interface, 0, error);
}

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

void
goodix_read_tls (FpDevice *dev, GoodixTlsCallback callback,
                 gpointer user_data)
{

  fp_dbg ("goodix_read_tls()");
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  if (priv->ack || priv->reply || priv->timeout)
    {
      callback (dev, NULL, 0, user_data,
                g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PENDING,
                                     "A Goodix command is still running"));
      return;
    }
  priv->generation++;
  priv->callback = callback;
  priv->user_data = user_data;
  priv->write_only = FALSE;
  priv->reply = TRUE;
  priv->cmd = 0;
  priv->response_flags = GOODIX_FLAGS_TLS;
  priv->timeout = fpi_device_add_timeout (dev, GOODIX_TIMEOUT,
                                          goodix_receive_timeout_cb, NULL, NULL);
}

static void
on_tls_successfully_established (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  g_autofree GoodixCallbackInfo *info =
    g_steal_pointer (&priv->tls_ready_callback);

  if (error)
    goodix_shutdown_tls (dev, NULL);
  if (info)
    ((GoodixNoneCallback) info->callback) (dev, info->user_data, error);
  else
    g_clear_error (&error);
}

enum goodix_tls_handshake_stages {
  TLS_HANDSHAKE_REQUEST,
  TLS_HANDSHAKE_FINISH,
  TLS_HANDSHAKE_NUM,
};

static void
tls_settled (FpDevice *dev, gpointer unused)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  priv->tls_settle_source = NULL;
  fpi_ssm_next_state (priv->tls_ssm);
}

static void
tls_final_flight_sent (FpDevice *dev, guint8 *data, guint16 length, gpointer ssm, GError *error)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else
    priv->tls_settle_source = fpi_device_add_timeout (dev, priv->tls_settle_ms,
                                                      tls_settled, NULL, NULL);
}

void
goodix_set_tls_settle_time (FpDevice *dev, guint milliseconds)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  priv->tls_settle_ms = milliseconds;
}

static void
on_goodix_tls_read_handshake (FpDevice *dev, guint8 *data, guint16 length,
                              gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  gboolean complete = FALSE;
  guint8 output[G_MAXUINT16];
  int size;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (goodix_tls_client_send (priv->tls_hop, data, length) != length)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                     "Failed to feed USB TLS record"));
      return;
    }
  if (!goodix_tls_server_handshake (priv->tls_hop, &complete, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* OpenSSL has synchronously produced the whole flight in the output BIO.
   * In particular, ChangeCipherSpec and Finished must reach fw 10062 together.
   * There is no worker-thread race or timing-based socket drain. */
  size = goodix_tls_client_recv (priv->tls_hop, output, sizeof (output));
  if (complete)
    {
      goodix_read_tls (dev, tls_final_flight_sent, ssm);
      priv->write_only = TRUE;
      if (size > 0)
        goodix_send_pack (dev, GOODIX_FLAGS_TLS, output, size);
      else
        goodix_receive_done (dev, NULL, 0, NULL);
    }
  else
    {
      goodix_read_tls (dev, on_goodix_tls_read_handshake, ssm);
      if (size > 0)
        goodix_send_pack (dev, GOODIX_FLAGS_TLS, output, size);
    }
}

static void
tls_handshake_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  priv->tls_ssm = NULL;
  g_clear_pointer (&priv->tls_settle_source, g_source_destroy);
  on_tls_successfully_established (dev, NULL, error);
}

static void
tls_handshake_ack (FpDevice *dev, gpointer ssm, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else
    fpi_ssm_next_state (ssm);
}

static void
tls_handshake_run (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case TLS_HANDSHAKE_REQUEST:
      goodix_send_request_tls_connection (dev, on_goodix_tls_read_handshake, ssm);
      break;

    case TLS_HANDSHAKE_FINISH:
      goodix_send_tls_successfully_established (dev, tls_handshake_ack, ssm);
      break;
    }
}

void
goodix_tls (FpDevice *dev, GoodixNoneCallback callback, gpointer user_data)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  GError *error = NULL;

  if (priv->tls_ssm || priv->tls_ready_callback)
    {
      callback (dev, user_data, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PENDING,
                                                     "A Goodix TLS handshake is already running"));
      return;
    }
  /* A finished session should have been shut down at deactivation. */
  if (priv->tls_hop)
    {
      fp_warn ("Replacing a leftover TLS session");
      goodix_shutdown_tls (dev, NULL);
    }
  priv->tls_hop = g_new0 (GoodixTlsServer, 1);
  priv->tls_ready_callback = g_new0 (GoodixCallbackInfo, 1);
  priv->tls_ready_callback->callback = G_CALLBACK (callback);
  priv->tls_ready_callback->user_data = user_data;

  if (!goodix_tls_server_init (priv->tls_hop, &error))
    {
      on_tls_successfully_established (dev, NULL, error);
      return;
    }
  priv->tls_ssm = fpi_ssm_new (dev, tls_handshake_run, TLS_HANDSHAKE_NUM);
  fpi_ssm_start (priv->tls_ssm, tls_handshake_done);
}

/* Deliver cancellation through the owner of the pending callback. In
 * particular, the TLS settle interval has an SSM but no USB command. */
gboolean
goodix_cancel_operation (FpDevice *dev)
{
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  if (priv->ack || priv->reply)
    {
      goodix_receive_done (dev, NULL, 0,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                "Goodix operation cancelled"));
    }
  else if (priv->tls_ssm)
    {
      g_clear_pointer (&priv->tls_settle_source, g_source_destroy);
      fpi_ssm_mark_failed (priv->tls_ssm,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                "Goodix TLS activation cancelled"));
    }
  else
    {
      return FALSE;
    }
  return TRUE;
}

gboolean
goodix_shutdown_tls (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->tls_hop)
    {
      gboolean rs = goodix_tls_server_deinit (priv->tls_hop, error);
      g_free (priv->tls_hop);
      priv->tls_hop = NULL;
      return rs;
    }
  return TRUE;
}
static void
goodix_tls_ready_image_handler (FpDevice *dev, guint8 *data,
                                guint16 length, gpointer user_data,
                                GError *error)
{
  g_autofree GoodixCallbackInfo *info = user_data;
  GoodixImageCallback callback = (GoodixImageCallback) info->callback;
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  g_autofree guint8 *buffer = NULL;
  int size;

  if (error)
    {
      callback (dev, NULL, 0, info->user_data, error);
      return;
    }
  if (!priv->tls_hop ||
      goodix_tls_client_send (priv->tls_hop, data, length) != length)
    {
      callback (dev, NULL, 0, info->user_data,
                g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "Failed to feed encrypted image"));
      return;
    }
  buffer = g_malloc (G_MAXUINT16);
  size = goodix_tls_server_receive (priv->tls_hop, buffer, G_MAXUINT16, &error);
  if (size <= 0)
    callback (dev, NULL, 0, info->user_data, error);
  else
    callback (dev, buffer, size, info->user_data, NULL);
}

void
goodix_tls_read_image (FpDevice *dev, GoodixImageCallback callback,
                       gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  g_return_if_fail (callback != NULL);
  cb_info = g_new (GoodixCallbackInfo, 1);

  cb_info->callback = G_CALLBACK (callback);
  cb_info->user_data = user_data;

  goodix_send_mcu_get_image (dev, goodix_tls_ready_image_handler, cb_info);
}

// ---- TLS SECTION END ----

static void
fpi_device_goodixtls_init (FpiDeviceGoodixTls *self)
{
}

static void
goodix_finalize (GObject *object)
{
  FpDevice *dev = FP_DEVICE (object);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));

  goodix_cancel_receive (dev);
  goodix_reset_state (dev);
  goodix_shutdown_tls (dev, NULL);
  g_clear_object (&priv->transfer_cancel_tkn);
  g_clear_pointer (&priv->tls_settle_source, g_source_destroy);
  G_OBJECT_CLASS (fpi_device_goodixtls_parent_class)->finalize (object);
}

static void
fpi_device_goodixtls_class_init (FpiDeviceGoodixTlsClass *class)
{
  G_OBJECT_CLASS (class)->finalize = goodix_finalize;
}
