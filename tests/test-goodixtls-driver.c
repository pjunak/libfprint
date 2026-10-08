/* Goodix command/lifecycle regressions. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "drivers_api.h"
#include <glib/gstdio.h>
#include "drivers/goodixtls/goodix.h"
#include <openssl/err.h>

/* Exercise the driver's private callbacks with USB ownership and libfprint
 * completion endpoints replaced. No reader, enrolled print or root is needed. */
static GError *reported_error;
static guint completions;
static guint retries;
static FpDeviceRetry last_retry;
static guint finger_off_reports;
static FpImage *captured_image;
static gboolean action_cancelled;
static FpiDeviceAction current_action;
static gboolean usb_claim_ok;
static guint usb_resets;

static gboolean
mock_action_is_cancelled (FpDevice *device)
{
  return action_cancelled;
}

static FpiDeviceAction
mock_current_action (FpDevice *device)
{
  return current_action;
}

static gboolean
mock_usb_reset (GUsbDevice *usb, GError **error)
{
  usb_resets++;
  return TRUE;
}

static void record_retry (FpImageDevice *device,
                          FpDeviceRetry  retry);
static void record_finger_status (FpImageDevice *device,
                                  gboolean       present);

static void
record_image (FpImageDevice *device, FpImage *image)
{
  g_assert_null (captured_image);
  captured_image = image;
}

#include "goodix-mock-usb.h"

static gboolean
usb_interface_failure (GUsbDevice *usb, gint interface,
                       GUsbDeviceClaimInterfaceFlags flags, GError **error)
{
  if (usb_claim_ok)
    return TRUE;
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "test USB interface failure");
  return FALSE;
}

static guint16
mock_usb_pid (GUsbDevice *usb)
{
  return 0x55a2;
}

static void
record_completion (FpImageDevice *device, GError *error)
{
  g_assert_null (reported_error);
  reported_error = error;
  completions++;
}

#define fpi_device_action_is_cancelled mock_action_is_cancelled
#define fpi_device_get_current_action mock_current_action
#define g_usb_device_reset mock_usb_reset
#define g_usb_device_claim_interface usb_interface_failure
#define g_usb_device_release_interface usb_interface_failure
#define g_usb_device_get_pid mock_usb_pid
#define fpi_image_device_open_complete record_completion
#define fpi_image_device_close_complete record_completion
#define fpi_image_device_activate_complete record_completion
#define fpi_image_device_deactivate_complete record_completion
#define fpi_image_device_session_error record_completion
#define fpi_image_device_retry_scan record_retry
#define fpi_image_device_report_finger_status record_finger_status
#define fpi_image_device_image_captured record_image
#include "drivers/goodixtls/goodix.c"
#undef FP_COMPONENT
#include "drivers/goodixtls/goodix55x4.c"

static void
record_retry (FpImageDevice *device, FpDeviceRetry retry)
{
  g_assert_true (retry == FP_DEVICE_RETRY_TOO_SHORT || retry == FP_DEVICE_RETRY_REMOVE_FINGER ||
                 retry == FP_DEVICE_RETRY_GENERAL);
  last_retry = retry;
  retries++;
}

static void
record_finger_status (FpImageDevice *device, gboolean present)
{
  if (!present)
    {
      g_assert_null (FPI_DEVICE_GOODIXTLS55X4 (device)->scan_ssm);
      finger_off_reports++;
    }
}

static FpDevice *
new_device (void)
{
  completions = 0;
  action_cancelled = FALSE;
  current_action = FPI_DEVICE_ACTION_NONE;
  usb_claim_ok = FALSE;
  usb_resets = 0;
  retries = 0;
  finger_off_reports = 0;
  g_assert_null (reported_error);
  g_assert_null (captured_image);
  return g_object_new (fpi_device_goodixtls55x4_get_type (), NULL);
}

static void
test_usb_errors (gconstpointer closing)
{
  g_autoptr(FpDevice) dev = new_device ();
  if (closing)
    dev_deinit (FP_IMAGE_DEVICE (dev));
  else
    dev_init (FP_IMAGE_DEVICE (dev));
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error (&reported_error);
}

static void
test_activation_error (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  tls_activation_complete (dev, NULL,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "TLS failed"));
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&reported_error);
}

static void
tls_completion (FpDevice *dev, gpointer user_data, GError *error)
{
  record_completion (FP_IMAGE_DEVICE (dev), error);
}

static void
test_handshake_error (gconstpointer ack)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "TLS timeout");
  priv->tls_ready_callback = g_new0 (GoodixCallbackInfo, 1);
  priv->tls_ready_callback->callback = G_CALLBACK (tls_completion);
  priv->tls_hop = g_new0 (GoodixTlsServer, 1);
  g_assert_true (goodix_tls_server_init (priv->tls_hop, NULL));

  if (ack)
    on_tls_successfully_established (dev, NULL, error);
  else
    tls_handshake_done (NULL, dev, error);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_null (priv->tls_ready_callback);
  g_assert_null (priv->tls_hop);
  g_clear_error (&reported_error);
}

static void
idle_state (FpiSsm *ssm, FpDevice *dev)
{
}

/* Exercise reporting and its synchronous transition without issuing USB reads. */
static void
report_scan_state (FpiSsm *ssm, FpDevice *dev)
{
  int state = fpi_ssm_get_cur_state (ssm);

  if (state == SCAN_REPORT || state == SCAN_DONE)
    scan_run_state (ssm, dev);
}

static void
test_short_frame (gconstpointer calibration)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint8 data = 0;
  self->scan_ssm = fpi_ssm_new (dev, idle_state, 1);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  if (calibration)
    on_scan_empty_img (dev, &data, 1, self->scan_ssm, NULL);
  else
    scan_on_read_img (dev, &data, 1, self->scan_ssm, NULL);
  g_assert_null (self->scan_ssm);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&reported_error);
}

static void
test_short_swipe (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint8 frame[GOODIX55X4_RAW_FRAME_SIZE];
  /* A lifted finger after too few stripes must emit a retry, not leave
   * fprintd waiting indefinitely for a result from the unfinished scan. */
  memset (frame, 0xff, sizeof (frame));
  self->swipe.present = TRUE;
  self->swipe.baseline = 2000;
  self->scan_ssm = fpi_ssm_new (dev, report_scan_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  scan_on_read_img (dev, frame, sizeof (frame), self->scan_ssm, NULL);
  g_assert_null (self->scan_ssm);
  g_assert_cmpuint (retries, ==, 1);
  g_assert_cmpuint (finger_off_reports, ==, 1);
  g_assert_cmpuint (completions, ==, 0);
}

static void
test_scan_done (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  self->scan_ssm = fpi_ssm_new (dev, idle_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  fpi_ssm_jump_to_state (self->scan_ssm, SCAN_DONE);
  scan_run_state (self->scan_ssm, dev);
  g_assert_null (self->scan_ssm);
  g_assert_cmpuint (finger_off_reports, ==, 1);
}

static void
test_sleep_failure (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  self->swipe.stripes = g_slist_prepend (NULL, g_malloc0 (32));
  self->swipe.n_stripes = 1;
  self->deactivating = TRUE;
  self->fdt_down_len = 26;
  priv->tls_hop = g_new0 (GoodixTlsServer, 1);
  g_assert_true (goodix_tls_server_init (priv->tls_hop, NULL));

  sleep_complete (NULL, dev,
                  g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "sleep timeout"));
  /* Deactivation still completes and cleans up, but a missed sleep must not
   * replace the result already reported for this attempt. */
  g_assert_cmpuint (completions, ==, 1);
  g_assert_no_error (reported_error);
  g_assert_null (self->swipe.stripes);
  g_assert_null (priv->tls_hop);
  g_assert_false (self->deactivating);
  g_assert_cmpuint (self->fdt_down_len, ==, 0);
}

static void
test_cancel_pending_scan (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  self->scan_ssm = fpi_ssm_new (dev, idle_state, 1);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  self->deactivating = TRUE;
  priv->reply = TRUE;
  priv->callback = check_none_cmd;
  priv->user_data = self->scan_ssm;
  goodix_receive_done (dev, NULL, 0,
                       g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled"));
  g_assert_null (self->scan_ssm);
  g_assert_null (priv->callback);
  g_assert_false (priv->reply);
  /* Cancellation belongs to deactivation, not a second session-error report. */
  g_assert_cmpuint (completions, ==, 0);
}

static void
test_old_transfer_cancelled (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  goodix_start_read_loop (dev);
  g_autoptr(GCancellable) old = g_object_ref (priv->transfer_cancel_tkn);
  goodix_cancel_receive (dev);
  goodix_start_read_loop (dev);
  g_assert_true (priv->inited);
  g_assert_true (pending_read->cancellable == old);
  g_assert_false (g_cancellable_is_cancelled (priv->transfer_cancel_tkn));
  drain_writes ();
  g_assert_true (pending_read->cancellable == priv->transfer_cancel_tkn);
  g_assert_true (priv->read_pending);
  goodix_cancel_receive (dev);
  drain_writes ();
  g_assert_null (pending_read);
  g_assert_false (priv->read_pending);
}

static void
packet_completion (FpDevice *dev, guint8 *data, guint16 length,
                   gpointer user_data, GError *error)
{
  record_completion (FP_IMAGE_DEVICE (dev), error);
}

static void
test_short_tls_header (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  guint8 payload[8] = {0};
  g_autofree guint8 *pack = NULL;
  guint32 length;
  goodix_encode_pack (GOODIX_FLAGS_TLS_DATA, payload, sizeof (payload), FALSE, &pack, &length);
  priv->reply = TRUE;
  priv->response_flags = GOODIX_FLAGS_TLS_DATA;
  priv->callback = packet_completion;
  goodix_receive_pack (dev, pack, length);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&reported_error);
}

static void
test_late_image_during_sleep (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  guint8 payload[32] = {0};
  g_autofree guint8 *pack = NULL;
  guint32 length;
  goodix_encode_pack (GOODIX_FLAGS_TLS_DATA, payload, sizeof (payload), FALSE, &pack, &length);
  /* A cancelled image can still be in the USB FIFO when sleep starts. It
   * must not finish sleep or be interpreted as its success status. */
  priv->cmd = GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE;
  priv->ack = TRUE;
  priv->callback = packet_completion;
  goodix_receive_pack (dev, pack, length);
  g_assert_cmpuint (completions, ==, 0);
  g_assert_true (priv->ack);
  goodix_reset_state (dev);
}

static void
test_frame_decode (void)
{
  guint8 raw[GOODIX55X4_RAW_FRAME_SIZE];
  Goodix55X4Pix decoded[GOODIX55X4_FRAME_SIZE];
  const guint8 packed[] = {0xab, 0x34, 0x78, 0x56, 0x9a, 0xcd};
  const guint16 pixels[] = {0xb34, 0x56a, 0xd78, 0x9ac};

  for (guint i = 0; i < sizeof (raw); i += sizeof (packed))
    memcpy (raw + i, packed, sizeof (packed));
  goodix_image_decode_frame (decoded, raw);
  for (guint i = 0; i < G_N_ELEMENTS (decoded); i++)
    g_assert_cmpuint (decoded[i], ==, pixels[i % 4]);
}

static void
test_idle_does_not_exhaust_swipe (void)
{
  GoodixSwipe swipe = {0};
  Goodix55X4Pix background[GOODIX55X4_FRAME_SIZE], frame[GOODIX55X4_FRAME_SIZE];

  for (guint i = 0; i < G_N_ELEMENTS (background); i++)
    background[i] = 2600;
  goodix_swipe_start (&swipe, 2600);
  for (guint i = 0; i < 2 * GOODIX_SWIPE_MAX_FRAMES; i++)
    {
      memcpy (frame, background, sizeof (frame));
      g_assert_cmpint (goodix_swipe_feed (&swipe, frame, background), ==, GOODIX_SWIPE_WAIT);
    }
  g_assert_cmpuint (swipe.n_frames, ==, 0);
  /* A slow placement must not pull the empty baseline down with it. */
  for (guint mean = 2580; mean >= 2260; mean -= 20)
    {
      for (guint i = 0; i < G_N_ELEMENTS (frame); i++)
        frame[i] = mean;
      g_assert_cmpint (goodix_swipe_feed (&swipe, frame, background), ==, GOODIX_SWIPE_WAIT);
    }
  for (guint i = 0; i < G_N_ELEMENTS (frame); i++)
    frame[i] = 2240;
  g_assert_cmpint (goodix_swipe_feed (&swipe, frame, background), ==, GOODIX_SWIPE_FINGER_ON);
  g_assert_cmpuint (swipe.n_frames, ==, 1);
  goodix_swipe_clear (&swipe);
}

static void
test_release_limit (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint8 raw[GOODIX55X4_RAW_FRAME_SIZE] = {0};
  self->swipe.baseline = 2600;
  self->release_frames = GOODIX_SWIPE_MAX_FRAMES;
  self->scan_ssm = fpi_ssm_new (dev, idle_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  fpi_ssm_jump_to_state (self->scan_ssm, SCAN_WAIT_RELEASE);
  scan_on_read_img (dev, raw, sizeof (raw), self->scan_ssm, NULL);
  g_assert_null (self->scan_ssm);
  g_assert_cmpuint (finger_off_reports, ==, 0);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&reported_error);
}

static void
test_read_holds_device (void)
{
  FpDevice *dev = new_device ();
  gpointer weak = dev;

  g_object_add_weak_pointer (G_OBJECT (dev), &weak);
  goodix_start_read_loop (dev);
  goodix_cancel_receive (dev);
  g_object_unref (dev);
  g_assert_nonnull (weak);
  drain_writes ();
  g_assert_null (weak);
}

static void
test_stationary_finger_limit (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint8 raw[GOODIX55X4_RAW_FRAME_SIZE] = {0};
  self->swipe.present = TRUE;
  self->swipe.baseline = 2000;
  self->swipe.n_frames = GOODIX_SWIPE_MAX_FRAMES;
  self->scan_ssm = fpi_ssm_new (dev, report_scan_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  scan_on_read_img (dev, raw, sizeof (raw), self->scan_ssm, NULL);
  g_assert_nonnull (self->scan_ssm);
  g_assert_cmpuint (retries, ==, 1);
  g_assert_cmpuint (completions, ==, 0);
  g_assert_cmpuint (finger_off_reports, ==, 0);
  g_clear_pointer (&self->delay, g_source_destroy);
  fpi_ssm_mark_completed (self->scan_ssm);
}

static void
test_stopped_swipe_keeps_stripes (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint8 raw[GOODIX55X4_RAW_FRAME_SIZE] = {0};
  self->swipe.present = TRUE;
  self->swipe.moving = TRUE;
  self->swipe.baseline = 2000;
  self->swipe.static_frames = GOODIX_SWIPE_STATIC_FRAMES - 1;
  for (guint i = 0; i < GOODIX_SWIPE_MIN_STRIPES; i++)
    self->swipe.stripes = g_slist_prepend (self->swipe.stripes, g_malloc0 (sizeof (struct fpi_frame) + GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT));
  self->swipe.n_stripes = GOODIX_SWIPE_MIN_STRIPES;
  self->scan_ssm = fpi_ssm_new (dev, report_scan_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  scan_on_read_img (dev, raw, sizeof (raw), self->scan_ssm, NULL);
  g_assert_null (captured_image); /* still touching the sensor */
  g_assert_nonnull (self->pending_image);
  g_assert_cmpuint (finger_off_reports, ==, 0);
  g_clear_pointer (&self->delay, g_source_destroy);
  fpi_ssm_jump_to_state (self->scan_ssm, SCAN_WAIT_RELEASE);
  memset (raw, 0xff, sizeof (raw)); /* lifted */
  scan_on_read_img (dev, raw, sizeof (raw), self->scan_ssm, NULL);
  g_assert_nonnull (captured_image);
  /* Blank stripes give no motion cue: one row each, no sideways drift. */
  g_assert_cmpuint (fp_image_get_width (captured_image), ==, GOODIX_SWIPE_IMAGE_W);
  g_assert_cmpuint (fp_image_get_height (captured_image), ==,
                    GOODIX55X4_SWIPE_FRAME_H + GOODIX_SWIPE_MIN_STRIPES - 1);
  g_assert_cmpuint (retries, ==, 0);
  g_assert_null (self->swipe.stripes);
  g_assert_cmpuint (finger_off_reports, ==, 1);
  g_assert_null (self->scan_ssm);
  g_clear_object (&captured_image);
}

static void
psk_result (FpDevice *dev, gboolean success, guint32 flags, guint8 *psk,
            guint16 length, gpointer user_data, GError *error)
{
  guint expected_length = GPOINTER_TO_UINT (user_data);

  if (!error)
    {
      g_assert_true (success);
      g_assert_cmphex (flags, ==, 0xbb020007);
      g_assert_cmpuint (length, ==, expected_length);
      for (guint i = 0; i < length; i++)
        g_assert_cmpuint (psk[i], ==, i);
    }
  else
    {
      g_assert_false (success);
      g_assert_null (psk);
    }
  record_completion (FP_IMAGE_DEVICE (dev), error);
}

static GoodixCallbackInfo *
psk_callback (guint expected_length)
{
  GoodixCallbackInfo *info = g_new0 (GoodixCallbackInfo, 1);

  info->callback = G_CALLBACK (psk_result);
  info->user_data = GUINT_TO_POINTER (expected_length);
  return info;
}

static void
test_psk_reply (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  guint8 reply[41] = {0, 0x07, 0x00, 0x02, 0xbb};
  for (guint i = 0; i < 32; i++)
    reply[9 + i] = i;
  for (guint length = 0; length <= 32; length++)
    {
      FP_WRITE_UINT32_LE (reply + 5, length);
      goodix_receive_preset_psk_read (dev, reply, 9 + length, psk_callback (length), NULL);
      g_assert_no_error (reported_error);
    }
  g_assert_cmpuint (completions, ==, 33);
}

static void
test_invalid_psk_reply (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  guint8 reply[41] = {0, 0x07, 0x00, 0x02, 0xbb, 32};
  for (guint length = 0; length < sizeof (reply); length++)
    {
      goodix_receive_preset_psk_read (dev, reply, length, psk_callback (32), NULL);
      g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_clear_error (&reported_error);
    }
  FP_WRITE_UINT32_LE (reply + 5, G_MAXUINT32);
  goodix_receive_preset_psk_read (dev, reply, sizeof (reply), psk_callback (32), NULL);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&reported_error);
}

static void
test_write_register_arguments (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GByteArray) bytes = g_byte_array_new ();
  const guint8 expected[] = {0, 0x34, 0x12, 0xcd, 0xab};
  sent_data = bytes;
  goodix_send_write_sensor_register (dev, 0x1234, 0xabcd, NULL, NULL);
  g_assert_cmpuint (bytes->len, ==, 0); /* No blocking writes. */
  drain_writes ();
  g_assert_cmpuint (bytes->len, ==, GOODIX_EP_OUT_MAX_BUF_SIZE);
  g_assert_cmphex (bytes->data[4], ==, GOODIX_CMD_WRITE_SENSOR_REGISTER);
  g_assert_cmpmem (bytes->data + 7, sizeof (expected), expected, sizeof (expected));
  goodix_receive_done (dev, NULL, 0, NULL);
  sent_data = NULL;
}

static void
test_coalesced_packets (gconstpointer fragment_size)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  guint8 ack[] = {GOODIX_CMD_MCU_GET_IMAGE, 1};
  guint8 image[12] = {0};
  g_autofree guint8 *protocol = NULL;
  g_autofree guint8 *ack_pack = NULL;
  g_autofree guint8 *image_pack = NULL;
  g_autoptr(GByteArray) stream = g_byte_array_new ();
  guint32 protocol_len, ack_len, image_len;

  goodix_encode_protocol (GOODIX_CMD_ACK, ack, sizeof (ack), TRUE, FALSE,
                          &protocol, &protocol_len);
  goodix_encode_pack (GOODIX_FLAGS_MSG_PROTOCOL, protocol, protocol_len, TRUE,
                      &ack_pack, &ack_len);
  goodix_encode_pack (GOODIX_FLAGS_TLS_DATA, image, sizeof (image), TRUE,
                      &image_pack, &image_len);
  g_byte_array_append (stream, ack_pack, ack_len);
  g_byte_array_append (stream, image_pack, image_len);
  priv->cmd = GOODIX_CMD_MCU_GET_IMAGE;
  priv->ack = TRUE;
  priv->reply = TRUE;
  priv->response_flags = GOODIX_FLAGS_TLS_DATA;
  priv->callback = packet_completion;

  guint chunk = GPOINTER_TO_UINT (fragment_size);
  for (guint offset = 0; offset < stream->len; offset += chunk)
    goodix_receive_pack (dev, stream->data + offset, MIN (chunk, stream->len - offset));
  g_assert_cmpuint (completions, ==, 1);
  g_assert_no_error (reported_error);
  g_assert_null (priv->receive_buffer);
}

static void
test_invalid_packet_checksum (gconstpointer inner)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  guint8 reply[] = {0xff, 0xff};
  g_autofree guint8 *protocol = NULL;
  g_autofree guint8 *packet = NULL;
  guint32 protocol_len, packet_len;

  goodix_encode_protocol (GOODIX_CMD_MCU_GET_IMAGE, reply, sizeof (reply), TRUE, FALSE,
                          &protocol, &protocol_len);
  if (inner)
    protocol[protocol_len - 1] ^= 0x40; /* Avoid the valid 0x88 no-checksum marker. */
  goodix_encode_pack (GOODIX_FLAGS_MSG_PROTOCOL, protocol, protocol_len, FALSE,
                      &packet, &packet_len);
  if (!inner)
    packet[3] ^= 1;
  priv->cmd = GOODIX_CMD_MCU_GET_IMAGE;
  priv->reply = TRUE;
  priv->callback = packet_completion;
  goodix_receive_pack (dev, packet, packet_len);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (priv->receive_buffer);
  g_clear_error (&reported_error);
}

static void
test_protocol_checksum_wrap (void)
{
  guint8 bytes[256];

  for (guint i = 0; i < G_N_ELEMENTS (bytes); i++)
    {
      g_autofree guint8 *encoded = NULL;
      g_autofree guint8 *decoded = NULL;
      guint32 encoded_len;
      guint16 decoded_len;
      guint8 cmd;
      gboolean valid, null_checksum;
      bytes[i] = i;
      goodix_encode_protocol (GOODIX_CMD_MCU_GET_IMAGE, bytes, i + 1, TRUE, FALSE,
                              &encoded, &encoded_len);
      g_assert_true (goodix_decode_protocol (encoded, encoded_len, &cmd, &decoded,
                                             &decoded_len, &valid, &null_checksum));
      g_assert_true (valid);
      g_assert_cmpmem (decoded, decoded_len, bytes, i + 1);
    }
}

static void
test_async_chunks (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GByteArray) bytes = g_byte_array_new ();
  guint8 payload[130] = {1};
  sent_data = bytes;
  goodix_send_protocol (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, payload, sizeof (payload),
                        NULL, TRUE, GOODIX_TIMEOUT, TRUE, packet_completion, NULL);
  g_assert_cmpuint (bytes->len, ==, 0);
  drain_writes ();
  g_assert_cmpuint (bytes->len, ==, 192);
  g_assert_cmpuint (completions, ==, 0);
  goodix_receive_done (dev, NULL, 0, NULL);
  g_assert_cmpuint (completions, ==, 1);
  sent_data = NULL;
}

static void
test_async_cancel_and_retry (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GByteArray) bytes = g_byte_array_new ();
  guint8 payload[130] = {1};
  sent_data = bytes;
  goodix_send_protocol (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, payload, sizeof (payload),
                        NULL, TRUE, GOODIX_TIMEOUT, TRUE, packet_completion, NULL);
  g_main_context_iteration (NULL, FALSE); /* Only the first chunk was sent. */
  goodix_receive_done (dev, NULL, 0,
                       g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancel"));
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&reported_error);
  goodix_send_nop (dev, tls_completion, NULL);
  drain_writes ();
  g_assert_cmpuint (bytes->len, ==, 128); /* First old chunk + new NOP only. */
  g_assert_cmpuint (completions, ==, 2);
  g_assert_no_error (reported_error);
  sent_data = NULL;
}

static void
test_async_failure (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  fail_write = TRUE;
  goodix_send_nop (dev, tls_completion, NULL);
  drain_writes ();
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&reported_error);
}

static void
test_finger_wait_ack_deadline (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  g_autoptr(GByteArray) bytes = g_byte_array_new ();
  guint8 payload[] = {1};
  guint8 ack[] = {GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, 1};
  sent_data = bytes;
  goodix_send_protocol (dev, ack[0], payload, sizeof (payload), NULL, TRUE,
                        0, TRUE, packet_completion, NULL);
  drain_writes ();
  g_assert_nonnull (priv->timeout); /* Even indefinite finger wait needs an ACK. */
  goodix_receive_ack (dev, ack, sizeof (ack), NULL, NULL);
  g_assert_null (priv->timeout);
  g_assert_true (priv->reply);
  goodix_receive_done (dev, NULL, 0, NULL);
  sent_data = NULL;
}

static void
test_activation_retry_limit (gconstpointer usb_error)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  GQuark domain = usb_error ? G_USB_DEVICE_ERROR : G_IO_ERROR;
  gint code = usb_error ? G_USB_DEVICE_ERROR_TIMED_OUT : G_IO_ERROR_TIMED_OUT;
  usb_claim_ok = TRUE;
  tls_activation_complete (dev, NULL, g_error_new_literal (domain, code, "timeout"));
  g_assert_cmpuint (self->activation_retries, ==, 1);
  g_assert_cmpuint (completions, ==, 0);
  g_assert_nonnull (self->delay);
  /* A second timeout resets the USB port once, then reinitializes after the
   * settle delay. No IN transfer is pending, so the reset is immediate. */
  tls_activation_complete (dev, NULL, g_error_new_literal (domain, code, "timeout"));
  g_assert_cmpuint (usb_resets, ==, 1);
  g_assert_true (self->usb_reset_done);
  g_assert_cmpuint (completions, ==, 0);
  g_assert_nonnull (self->delay);
  /* Exhausted budget must complete instead of scheduling another retry. */
  tls_activation_complete (dev, NULL, g_error_new_literal (domain, code, "timeout"));
  g_assert_cmpuint (usb_resets, ==, 1);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_null (self->delay);
  g_assert_error (reported_error, domain, code);
  g_clear_error (&reported_error);
}

static void
test_activation_retry_classes (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  usb_claim_ok = TRUE;
  /* A stale packet from a cancelled session is retried, but is not a reason
   * to reset the port: the MCU was still answering. */
  tls_activation_complete (dev, NULL, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                           "Invalid Goodix packet checksum"));
  g_assert_cmpuint (completions, ==, 0);
  tls_activation_complete (dev, NULL, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                           "Invalid Goodix packet checksum"));
  g_assert_cmpuint (usb_resets, ==, 0);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&reported_error);

  /* A sensor paired with another key fails at once. */
  self->activation_retries = 0;
  self->activating = TRUE;
  tls_activation_complete (dev, NULL, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                                           "Invalid device PSK"));
  g_assert_cmpuint (completions, ==, 2);
  g_assert_cmpuint (self->activation_retries, ==, 0);
  g_assert_null (self->delay);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&reported_error);
}

static gboolean
reset_test_watchdog (gpointer unused)
{
  g_error ("USB reset did not run");
  return G_SOURCE_REMOVE;
}

static void
test_usb_reset_waits_for_read (gconstpointer read_never_finishes)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  usb_claim_ok = TRUE;
  self->activation_retries = 1;
  priv->read_pending = TRUE;
  tls_activation_complete (dev, NULL, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "timeout"));
  /* The cancelled IN transfer has not called back yet; never reset under it. */
  g_assert_cmpuint (usb_resets, ==, 0);
  g_assert_nonnull (self->delay);
  if (!read_never_finishes)
    priv->read_pending = FALSE;
  guint guard = g_timeout_add_seconds (3, reset_test_watchdog, NULL);
  while (!usb_resets && !completions)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (guard);
  if (read_never_finishes)
    {
      g_assert_cmpuint (usb_resets, ==, 0);
      g_assert_cmpuint (completions, ==, 1);
      g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
      g_clear_error (&reported_error);
      priv->read_pending = FALSE;
    }
  else
    {
      g_assert_cmpuint (usb_resets, ==, 1);
      g_assert_cmpuint (completions, ==, 0);
      g_assert_nonnull (self->delay); /* settle, then reinitialize */
    }
}

static void
read_completion (FpDevice *dev, guint8 *data, guint16 length, gpointer user_data, GError *error)
{
  record_completion (FP_IMAGE_DEVICE (dev), error);
}

static void
test_busy_tls_requests (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  GoodixCallbackInfo *pending = g_new0 (GoodixCallbackInfo, 1);

  /* A second handshake while one runs is refused, not asserted. */
  priv->tls_ready_callback = pending;
  goodix_tls (dev, tls_completion, NULL);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&reported_error);
  g_assert_true (priv->tls_ready_callback == pending);
  g_clear_pointer (&priv->tls_ready_callback, g_free);

  /* So is a TLS read while a command waits for its reply. */
  priv->reply = TRUE;
  goodix_read_tls (dev, read_completion, NULL);
  g_assert_cmpuint (completions, ==, 2);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_PENDING);
  g_clear_error (&reported_error);
  priv->reply = FALSE;
}

static void
write_removable (const gchar *root, const gchar *name, const gchar *value)
{
  g_autofree gchar *dir = g_build_filename (root, "bus", "usb", "devices", name, NULL);
  g_autofree gchar *file = g_build_filename (dir, "removable", NULL);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (file, value, -1, NULL));
}

static void
remove_tree (const gchar *path)
{
  g_autoptr(GDir) dir = g_dir_open (path, 0, NULL);
  const gchar *name;
  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree gchar *child = g_build_filename (path, name, NULL);
      if (g_file_test (child, G_FILE_TEST_IS_DIR))
        remove_tree (child);
      else
        g_remove (child);
    }
  g_rmdir (path);
}

static void
test_builtin_port_policy (void)
{
  g_autofree gchar *root = g_dir_make_tmp ("goodix-sysfs-XXXXXX", NULL);
  const guint8 direct[] = {3};
  const guint8 behind_hub[] = {1, 4};
  g_autofree gchar *external = NULL;
  g_autofree gchar *hub = NULL;

  g_autoptr(FpDevice) dev = NULL;

  /* The built-in reader: a fixed port on the root hub. */
  write_removable (root, "3-3", "fixed\n");
  g_assert_null (goodix_removable_link (root, 3, direct, 1));
  /* A look-alike plugged into an external connector. */
  write_removable (root, "3-3", "removable\n");
  external = goodix_removable_link (root, 3, direct, 1);
  g_assert_cmpstr (external, ==, "3-3");
  /* Behind a hub on an external connector; the reader's own port on that
   * hub is undescribed, so the hub's port has to decide. */
  write_removable (root, "3-1", "removable\n");
  write_removable (root, "3-1.4", "unknown\n");
  hub = goodix_removable_link (root, 3, behind_hub, 2);
  g_assert_cmpstr (hub, ==, "3-1");
  /* Firmware that describes no ports, or no sysfs at all: nothing to judge. */
  write_removable (root, "3-1", "unknown\n");
  g_assert_null (goodix_removable_link (root, 3, behind_hub, 2));
  g_assert_null (goodix_removable_link ("/nonexistent", 3, direct, 1));
  remove_tree (root);

  /* Emulated devices have no USB device to check. */
  dev = new_device ();
  g_assert_true (goodix_check_builtin_port (dev, NULL));
}

static void
test_close_waits_for_read (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  usb_claim_ok = TRUE;
  priv->read_pending = TRUE;
  /* The cancelled read has not called back; closing must not finish yet. */
  dev_deinit (FP_IMAGE_DEVICE (dev));
  g_assert_cmpuint (completions, ==, 0);
  g_assert_nonnull (self->delay);
  priv->read_pending = FALSE;
  guint guard = g_timeout_add_seconds (3, reset_test_watchdog, NULL);
  while (!completions)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (guard);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_no_error (reported_error);
  g_assert_null (self->delay);
}

static void
fail_scan (FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);

  self->scan_ssm = fpi_ssm_new (dev, idle_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  fpi_ssm_jump_to_state (self->scan_ssm, SCAN_READ);
  fpi_ssm_mark_failed (self->scan_ssm, error);
  g_assert_null (self->scan_ssm);
}

static void
test_transient_scan_retry (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  current_action = FPI_DEVICE_ACTION_VERIFY;
  /* A lost TLS session costs one more swipe, at most twice per open. */
  for (guint i = 1; i <= GOODIX_TRANSIENT_RETRIES; i++)
    {
      fail_scan (dev, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "lost TLS"));
      g_assert_cmpuint (retries, ==, i);
      g_assert_cmpint (last_retry, ==, FP_DEVICE_RETRY_GENERAL);
      g_assert_cmpuint (completions, ==, 0);
    }
  fail_scan (dev, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "lost TLS"));
  g_assert_cmpuint (retries, ==, GOODIX_TRANSIENT_RETRIES);
  g_assert_cmpuint (completions, ==, 1);
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
  g_clear_error (&reported_error);

  /* Opening the device again restores the budget. */
  dev_init (FP_IMAGE_DEVICE (dev));
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_error (&reported_error);
  g_assert_cmpuint (self->transient_retries, ==, 0);

  /* Not retried: the device is gone, the image was already reported (the
   * core has a result), or the action keeps one activation (enrollment). */
  fail_scan (dev, g_error_new_literal (G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE, "gone"));
  g_assert_error (reported_error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE);
  g_clear_error (&reported_error);
  self->image_reported = TRUE;
  fail_scan (dev, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "finger remained"));
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&reported_error);
  self->image_reported = FALSE;
  current_action = FPI_DEVICE_ACTION_ENROLL;
  fail_scan (dev, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "lost TLS"));
  g_assert_error (reported_error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
  g_clear_error (&reported_error);
  g_assert_cmpuint (retries, ==, GOODIX_TRANSIENT_RETRIES);
}

static void
test_rebaseline_after_held_finger (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint8 empty[GOODIX55X4_RAW_FRAME_SIZE];
  memset (empty, 0xaa, sizeof (empty)); /* valid empty-sensor calibration */
  FpiSsm *ssm = fpi_ssm_new (dev, idle_state, SCAN_NUM_STATES);
  fpi_ssm_start (ssm, scan_complete);

  /* A prompt finger-up answer keeps the baseline. */
  self->fdt_down_len = 26;
  fpi_ssm_jump_to_state (ssm, SCAN_WAIT_EMPTY);
  self->wait_empty_started = g_get_monotonic_time ();
  wait_empty_done (dev, NULL, 0, ssm, NULL);
  g_assert_false (self->rebaselined);
  g_assert_cmpuint (self->fdt_down_len, ==, 26);

  /* A slow one means the baseline was measured with a finger on the sensor:
   * after the empty image, configure finger detection again. */
  fpi_ssm_jump_to_state (ssm, SCAN_WAIT_EMPTY);
  self->wait_empty_started = g_get_monotonic_time () - 2 * GOODIX_REBASELINE_AFTER_MS * 1000;
  wait_empty_done (dev, NULL, 0, ssm, NULL);
  g_assert_true (self->rebaselined);
  g_assert_cmpuint (self->fdt_down_len, ==, 0);
  g_assert_cmpint (fpi_ssm_get_cur_state (ssm), ==, SCAN_NAV);
  fpi_ssm_jump_to_state (ssm, SCAN_CALIBRATE);
  on_scan_empty_img (dev, empty, sizeof (empty), ssm, NULL);
  g_assert_cmpint (fpi_ssm_get_cur_state (ssm), ==, SCAN_QUERY_MCU);
  g_assert_false (self->calibrated);

  /* Only once per activation, even if the finger rests again. */
  self->fdt_down_len = 26; /* new baseline from the repeated mode switch */
  fpi_ssm_jump_to_state (ssm, SCAN_WAIT_EMPTY);
  self->wait_empty_started = g_get_monotonic_time () - 2 * GOODIX_REBASELINE_AFTER_MS * 1000;
  wait_empty_done (dev, NULL, 0, ssm, NULL);
  g_assert_cmpuint (self->fdt_down_len, ==, 26);
  fpi_ssm_jump_to_state (ssm, SCAN_CALIBRATE);
  on_scan_empty_img (dev, empty, sizeof (empty), ssm, NULL);
  g_assert_true (self->calibrated);
  g_assert_cmpint (fpi_ssm_get_cur_state (ssm), ==, SCAN_WAIT_FINGER);
  fpi_ssm_mark_completed (ssm);
  g_assert_cmpuint (completions, ==, 0);
}

static void
test_idle_rearms_finger_detection (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint8 empty[GOODIX55X4_RAW_FRAME_SIZE];
  memset (empty, 0xaa, sizeof (empty));
  goodix_swipe_start (&self->swipe, 2730);
  self->scan_ssm = fpi_ssm_new (dev, idle_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
  fpi_ssm_jump_to_state (self->scan_ssm, SCAN_READ);
  /* Finger detection fired, but no contact follows: stream for a bounded
   * time, then return to the low-power finger wait. */
  for (guint i = 1; i < GOODIX_IDLE_REARM_FRAMES; i++)
    {
      scan_on_read_img (dev, empty, sizeof (empty), self->scan_ssm, NULL);
      g_assert_cmpint (fpi_ssm_get_cur_state (self->scan_ssm), ==, SCAN_READ);
      g_assert_nonnull (self->delay);
      g_clear_pointer (&self->delay, g_source_destroy);
    }
  scan_on_read_img (dev, empty, sizeof (empty), self->scan_ssm, NULL);
  g_assert_cmpint (fpi_ssm_get_cur_state (self->scan_ssm), ==, SCAN_WAIT_FINGER);
  g_assert_null (self->delay);
  g_assert_cmpuint (completions, ==, 0);
  g_assert_cmpuint (retries, ==, 0);
  fpi_ssm_mark_completed (self->scan_ssm);
}

static void
replayed_firmware (FpDevice *dev, gchar *firmware, gpointer expected, GError *error)
{
  g_assert_no_error (error);
  g_assert_cmpstr (firmware, ==, expected);
  completions++;
}

static GByteArray *
hex_bytes (const gchar *hex)
{
  GByteArray *bytes = g_byte_array_new ();

  g_assert_cmpuint (strlen (hex) % 2, ==, 0);
  for (gsize i = 0; hex[i]; i += 2)
    {
      gint high = g_ascii_xdigit_value (hex[i]), low = g_ascii_xdigit_value (hex[i + 1]);
      g_assert_cmpint (high, >=, 0);
      g_assert_cmpint (low, >=, 0);
      guint8 value = (high << 4) | low;
      g_byte_array_append (bytes, &value, 1);
    }
  return bytes;
}

static void
test_firmware_replay (void)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GKeyFile) fixture = g_key_file_new ();
  g_autofree gchar *path = g_build_filename (g_getenv ("MESON_SOURCE_ROOT"), "tests", "goodix-wire-replay.ini", NULL);
  g_assert_true (g_key_file_load_from_file (fixture, path, G_KEY_FILE_NONE, NULL));
  g_autofree gchar *tx = g_key_file_get_string (fixture, "firmware", "request", NULL);
  g_autofree gchar *rx = g_key_file_get_string (fixture, "firmware", "reply", NULL);
  g_autofree gchar *firmware = g_key_file_get_string (fixture, "firmware", "firmware", NULL);
  g_autoptr(GByteArray) expected = hex_bytes (tx);
  g_autoptr(GByteArray) response = hex_bytes (rx);
  g_autoptr(GByteArray) writes = g_byte_array_new ();
  sent_data = writes;
  goodix_send_firmware_version (dev, replayed_firmware, firmware);
  drain_writes ();
  g_assert_cmpmem (writes->data, writes->len, expected->data, expected->len);
  for (guint i = 0; i < response->len; i += 7)
    goodix_receive_pack (dev, response->data + i, MIN (7, response->len - i));
  g_assert_cmpuint (completions, ==, 1);
  sent_data = NULL;
}

/* Replay the cleartext initialization exactly, then replace recorded TLS with
 * a live sensor peer. Only this test binary contains the peer: production TLS
 * keeps fresh randomness, including when FP_DEVICE_EMULATION is set. */
static GKeyFile *setup_trace;
static guint trace_step, trace_steps, replay_fragment;
static GByteArray *outgoing;
static SSL *sensor;
static guint8 replay_image[GOODIX55X4_RAW_FRAME_SIZE];

static void
queue_rx (const guint8 *data, gsize size)
{
  for (gsize offset = 0; offset < size; offset += replay_fragment)
    g_queue_push_tail (&incoming, g_bytes_new (data + offset, MIN (replay_fragment, size - offset)));
  schedule_mock_read (NULL, NULL);
}

static void
queue_pack (guint8 flags, const guint8 *data, guint16 size)
{
  g_autofree guint8 *packet = NULL;
  guint32 length;

  goodix_encode_pack (flags, (guint8 *) data, size, TRUE, &packet, &length);
  queue_rx (packet, length);
}

static void
queue_ack (guint8 cmd)
{
  guint8 ack[] = {cmd, 1};
  g_autofree guint8 *protocol = NULL;
  guint32 length;

  goodix_encode_protocol (GOODIX_CMD_ACK, ack, sizeof (ack), TRUE, FALSE, &protocol, &length);
  queue_pack (GOODIX_FLAGS_MSG_PROTOCOL, protocol, length);
}

static unsigned int
sensor_psk (SSL *ssl, const char *hint, char *identity, unsigned int identity_len,
            unsigned char *psk, unsigned int psk_len)
{
  g_strlcpy (identity, "replay sensor", identity_len);
  g_assert_cmpuint (psk_len, >=, 32);
  memset (psk, 0, 32);
  return 32;
}

static void
sensor_handshake (const guint8 *data, guint size)
{
  if (size)
    g_assert_cmpint (BIO_write (SSL_get_rbio (sensor), data, size), ==, size);
  ERR_clear_error ();
  int result = SSL_do_handshake (sensor);
  if (result != 1)
    g_assert_cmpint (SSL_get_error (sensor, result), ==, SSL_ERROR_WANT_READ);
  guint8 buffer[8192];
  int length;
  while ((length = BIO_read (SSL_get_wbio (sensor), buffer, sizeof (buffer))) > 0)
    queue_pack (GOODIX_FLAGS_TLS, buffer, length);
}

static void
replay_write (FpDevice *dev, const guint8 *data, gsize size)
{
  g_byte_array_append (outgoing, data, size);
  GoodixPacket packet;
  gint parsed = goodix_packet_peek (outgoing->data, outgoing->len, &packet);
  g_assert_cmpint (parsed, >=, 0);
  if (!parsed)
    return;

  if (trace_step < trace_steps)
    {
      g_autofree gchar *group = g_strdup_printf ("exchange-%u", trace_step++);
      g_autofree gchar *tx = g_key_file_get_string (setup_trace, group, "request", NULL);
      g_autofree gchar *rx = g_key_file_get_string (setup_trace, group, "reply", NULL);
      g_assert_nonnull (tx);
      g_assert_nonnull (rx);
      g_autoptr(GByteArray) expected = hex_bytes (tx);
      g_autoptr(GByteArray) response = hex_bytes (rx);
      g_test_message ("Replaying %s", group);
      g_assert_cmpmem (outgoing->data, outgoing->len, expected->data, expected->len);
      queue_rx (response->data, response->len);
    }
  else if (packet.flags == GOODIX_FLAGS_TLS)
    {
      sensor_handshake (packet.payload, packet.length);
    }
  else
    {
      g_assert_cmpuint (packet.flags, ==, GOODIX_FLAGS_MSG_PROTOCOL);
      g_assert_cmpuint (packet.length, >=, 4);
      guint8 cmd = packet.payload[0];
      queue_ack (cmd);
      switch (cmd)
        {
        case GOODIX_CMD_REQUEST_TLS_CONNECTION:
          sensor_handshake (NULL, 0);
          break;

        case GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED:
          g_assert_true (SSL_is_init_finished (sensor));
          break;

        case GOODIX_CMD_QUERY_MCU_STATE:
        case GOODIX_CMD_MCU_SWITCH_TO_FDT_UP:
        case GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE:
          {
            guint8 payload[] = {0, 0, 0, 0, 0x60, 1, 0x62, 1};
            g_autofree guint8 *protocol = NULL;
            guint32 length;
            goodix_encode_protocol (cmd, payload, sizeof (payload), TRUE, FALSE, &protocol, &length);
            queue_pack (GOODIX_FLAGS_MSG_PROTOCOL, protocol, length);
            break;
          }

        case GOODIX_CMD_NAV_0:
          break;

        case GOODIX_CMD_MCU_GET_IMAGE:
          {
            /* Two TLS records exercise record coalescing inside one USB packet. */
            g_assert_cmpint (SSL_write (sensor, replay_image, 5000), ==, 5000);
            g_assert_cmpint (SSL_write (sensor, replay_image + 5000, sizeof (replay_image) - 5000),
                             ==, sizeof (replay_image) - 5000);
            guint8 encrypted[20000] = {0};
            int length = BIO_read (SSL_get_wbio (sensor), encrypted + 9, sizeof (encrypted) - 9);
            g_assert_cmpint (length, >, 0);
            queue_pack (GOODIX_FLAGS_TLS_DATA, encrypted, length + 9);
            break;
          }

        default:
          g_error ("Unexpected command after setup replay: 0x%02x", cmd);
        }
    }
  g_byte_array_set_size (outgoing, 0);
}

static gboolean
replay_watchdog (gpointer unused)
{
  g_error ("Replay did not complete within five seconds");
  return G_SOURCE_REMOVE;
}

static void
await_completions (guint expected)
{
  guint timeout = g_timeout_add_seconds (5, replay_watchdog, NULL);

  while (completions < expected)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout);
  g_assert_cmpuint (completions, ==, expected);
  g_assert_no_error (reported_error);
}

static void
replayed_image (FpDevice *dev, guint8 *data, guint16 size, gpointer unused, GError *error)
{
  g_assert_no_error (error);
  g_assert_cmpmem (data, size, replay_image, sizeof (replay_image));
  completions++;
}

static void
test_setup_replay (gconstpointer fragment)
{
  g_autoptr(FpDevice) dev = new_device ();
  g_autoptr(GKeyFile) fixture = g_key_file_new ();
  const gchar *override = g_getenv ("GOODIX_TEST_SETUP_TRACE");
  g_autofree gchar *path = override ? g_strdup (override) :
                           g_build_filename (g_getenv ("MESON_SOURCE_ROOT"), "tests", "goodix-setup-replay.ini", NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true (g_key_file_load_from_file (fixture, path, G_KEY_FILE_NONE, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_key_file_get_integer (fixture, "replay", "format", NULL), ==, 1);
  trace_steps = g_key_file_get_integer (fixture, "replay", "exchanges", NULL);
  g_assert_cmpuint (trace_steps, ==, 8);
  g_autofree gchar *provenance = g_key_file_get_string (fixture, "replay", "provenance", NULL);
  g_assert_nonnull (provenance);
  g_test_message ("Setup fixture: %s (%s); TLS and image are synthetic", path, provenance);
  setup_trace = fixture;
  trace_step = 0;
  replay_fragment = GPOINTER_TO_UINT (fragment);
  g_autoptr(GByteArray) writes = g_byte_array_new ();
  g_autoptr(GByteArray) tx = g_byte_array_new ();
  sent_data = writes;
  outgoing = tx;
  observe_write = replay_write;
  SSL_CTX *ctx = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (ctx);
  SSL_CTX_set_security_level (ctx, 0);
  g_assert_cmpint (SSL_CTX_set_cipher_list (ctx, "PSK-AES128-CBC-SHA256"), ==, 1);
  g_assert_cmpint (SSL_CTX_set_min_proto_version (ctx, TLS1_2_VERSION), ==, 1);
  g_assert_cmpint (SSL_CTX_set_max_proto_version (ctx, TLS1_2_VERSION), ==, 1);
  SSL_CTX_set_psk_client_callback (ctx, sensor_psk);

  /* Cancel a pending USB read and immediately reactivate the same device. */
  for (guint cycle = 0; cycle < 2; cycle++)
    {
      trace_step = 0;
      sensor = SSL_new (ctx);
      g_assert_nonnull (sensor);
      BIO *input = BIO_new (BIO_s_mem ()), *output = BIO_new (BIO_s_mem ());
      g_assert_nonnull (input);
      g_assert_nonnull (output);
      BIO_set_mem_eof_return (input, -1);
      BIO_set_mem_eof_return (output, -1);
      SSL_set_bio (sensor, input, output);
      SSL_set_connect_state (sensor);
      /* Activation now finishes empty-sensor calibration before reporting
       * readiness. Packed value 0xaaa is a valid empty test baseline. */
      memset (replay_image, 0xaa, sizeof (replay_image));
      dev_activate (FP_IMAGE_DEVICE (dev));
      await_completions (cycle * 2 + 1);
      g_assert_cmpuint (trace_step, ==, trace_steps);
      for (guint i = 0; i < sizeof (replay_image); i++)
        replay_image[i] = (i + cycle) % 251;
      goodix_tls_read_image (dev, replayed_image, NULL);
      await_completions (cycle * 2 + 2);
      drain_writes ();
      g_assert_true (g_queue_is_empty (&incoming));
      goodix_cancel_receive (dev);
      goodix_shutdown_tls (dev, NULL);
      goodix_reset_state (dev);
      drain_writes ();
      g_assert_null (pending_read);
      g_assert_cmpuint (outgoing->len, ==, 0);
      SSL_free (sensor);
      sensor = NULL;
    }
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (FPI_DEVICE_GOODIXTLS (dev));
  g_clear_object (&priv->transfer_cancel_tkn);
  SSL_CTX_free (ctx);
  setup_trace = NULL;
  observe_write = NULL;
  sent_data = outgoing = NULL;
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/goodixtls/driver/open-error", NULL, test_usb_errors);
  g_test_add_data_func ("/goodixtls/driver/close-error", GINT_TO_POINTER (1), test_usb_errors);
  g_test_add_func ("/goodixtls/driver/activation-error", test_activation_error);
  g_test_add_data_func ("/goodixtls/driver/handshake-error", NULL, test_handshake_error);
  g_test_add_data_func ("/goodixtls/driver/tls-ack-error", GINT_TO_POINTER (1), test_handshake_error);
  g_test_add_func ("/goodixtls/driver/sleep-error", test_sleep_failure);
  g_test_add_func ("/goodixtls/driver/cancel-pending-scan", test_cancel_pending_scan);
  g_test_add_func ("/goodixtls/driver/old-transfer-cancelled", test_old_transfer_cancelled);
  g_test_add_func ("/goodixtls/driver/short-tls-header", test_short_tls_header);
  g_test_add_func ("/goodixtls/driver/late-image-during-sleep", test_late_image_during_sleep);
  g_test_add_data_func ("/goodixtls/driver/short-calibration", GINT_TO_POINTER (1), test_short_frame);
  g_test_add_data_func ("/goodixtls/driver/short-image", NULL, test_short_frame);
  g_test_add_func ("/goodixtls/driver/short-swipe", test_short_swipe);
  g_test_add_func ("/goodixtls/driver/scan-done", test_scan_done);
  g_test_add_func ("/goodixtls/driver/decode-frame", test_frame_decode);
  g_test_add_func ("/goodixtls/driver/idle-swipe-budget", test_idle_does_not_exhaust_swipe);
  g_test_add_func ("/goodixtls/driver/release-limit", test_release_limit);
  g_test_add_func ("/goodixtls/driver/read-keeps-device-alive", test_read_holds_device);
  g_test_add_func ("/goodixtls/driver/stationary-finger-limit", test_stationary_finger_limit);
  g_test_add_func ("/goodixtls/driver/stopped-swipe-keeps-stripes", test_stopped_swipe_keeps_stripes);
  g_test_add_func ("/goodixtls/driver/psk-reply", test_psk_reply);
  g_test_add_func ("/goodixtls/driver/invalid-psk-reply", test_invalid_psk_reply);
  g_test_add_func ("/goodixtls/driver/write-register-arguments", test_write_register_arguments);
  g_test_add_data_func ("/goodixtls/driver/coalesced-packets", GUINT_TO_POINTER (128), test_coalesced_packets);
  g_test_add_data_func ("/goodixtls/driver/fragmented-packets", GUINT_TO_POINTER (7), test_coalesced_packets);
  g_test_add_data_func ("/goodixtls/driver/bytewise-packets", GUINT_TO_POINTER (1), test_coalesced_packets);
  g_test_add_data_func ("/goodixtls/driver/bad-packet-checksum", NULL, test_invalid_packet_checksum);
  g_test_add_data_func ("/goodixtls/driver/bad-protocol-checksum", GINT_TO_POINTER (1), test_invalid_packet_checksum);
  g_test_add_func ("/goodixtls/driver/checksum-wrap", test_protocol_checksum_wrap);
  g_test_add_func ("/goodixtls/driver/async-chunks", test_async_chunks);
  g_test_add_func ("/goodixtls/driver/async-cancel-retry", test_async_cancel_and_retry);
  g_test_add_func ("/goodixtls/driver/async-failure", test_async_failure);
  g_test_add_func ("/goodixtls/driver/finger-wait-ack-deadline", test_finger_wait_ack_deadline);
  g_test_add_func ("/goodixtls/driver/firmware-replay", test_firmware_replay);
  g_test_add_data_func ("/goodixtls/driver/setup-replay", GUINT_TO_POINTER (65536), test_setup_replay);
  g_test_add_data_func ("/goodixtls/driver/setup-replay-fragmented", GUINT_TO_POINTER (7), test_setup_replay);
  g_test_add_data_func ("/goodixtls/driver/activation-retry-limit", NULL, test_activation_retry_limit);
  g_test_add_data_func ("/goodixtls/driver/usb-activation-retry-limit", GINT_TO_POINTER (1), test_activation_retry_limit);
  g_test_add_func ("/goodixtls/driver/activation-retry-classes", test_activation_retry_classes);
  g_test_add_data_func ("/goodixtls/driver/usb-reset-waits-for-read", NULL, test_usb_reset_waits_for_read);
  g_test_add_data_func ("/goodixtls/driver/usb-reset-read-stuck", GINT_TO_POINTER (1), test_usb_reset_waits_for_read);
  g_test_add_func ("/goodixtls/driver/close-waits-for-read", test_close_waits_for_read);
  g_test_add_func ("/goodixtls/driver/builtin-port-policy", test_builtin_port_policy);
  g_test_add_func ("/goodixtls/driver/busy-tls-requests", test_busy_tls_requests);
  g_test_add_func ("/goodixtls/driver/transient-scan-retry", test_transient_scan_retry);
  g_test_add_func ("/goodixtls/driver/rebaseline-after-held-finger", test_rebaseline_after_held_finger);
  g_test_add_func ("/goodixtls/driver/idle-rearms-finger-detection", test_idle_rearms_finger_detection);
  return g_test_run ();
}
