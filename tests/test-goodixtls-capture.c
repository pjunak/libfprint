/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Public-API integration test. Only the USB boundary is mocked: the real
 * driver, TLS, swipe processing, image-device state machine and NBIS run. */
#include "drivers_api.h"
#include "goodix-mock-usb.h"
#include <openssl/err.h>
#include <cairo.h>

static gboolean
mock_interface (GUsbDevice *usb, gint interface, GUsbDeviceClaimInterfaceFlags flags, GError **error)
{
  return TRUE;
}

static GUsbDevice *
mock_usb_device (FpDevice *device)
{
  return NULL;
}
static guint16
mock_pid (GUsbDevice *usb)
{
  return 0x55a2;
}
static guint usb_resets;
static gboolean
mock_usb_reset (GUsbDevice *usb, GError **error)
{
  usb_resets++;
  return TRUE;
}

#define g_usb_device_reset mock_usb_reset
#define g_usb_device_claim_interface mock_interface
#define g_usb_device_release_interface mock_interface
#define g_usb_device_get_pid mock_pid
#define fpi_device_get_usb_device mock_usb_device
#include "drivers/goodixtls/goodix.c"
#undef FP_COMPONENT
#include "drivers/goodixtls/goodix55x4.c"

static SSL_CTX *sensor_ctx;
static SSL *sensor;
static GByteArray *outgoing;
static guint fragment_size;
static guint calibration_count, swipe_number, frame_number, held_frames;
static guint sleep_commands, led_commands, mode_commands;
static gboolean calibration_next, short_swipe, hold_finger, sensor_has_finger;
static gboolean stop_after_ack, drop_mode_reply, fail_sleep, lost_tls, invalid_baseline;
static gboolean cancel_settle, lost_tls_swipe;
/* Finger-up answers this late while a finger rests; 0 answers at once. */
static guint slow_fdt_up_ms, slow_fdt_up_count;
/* Empty frames streamed after finger detection fires, before any contact. */
static guint idle_frames_before_touch;
static guint8 cancel_on_command;
static GCancellable *test_cancel;
static cairo_surface_t *fingerprint;

static unsigned int
sensor_psk (SSL *ssl, const char *hint, char *identity, unsigned int identity_size,
            unsigned char *psk, unsigned int size)
{
  g_assert_cmpuint (size, >=, 32);
  g_strlcpy (identity, "Client_identity", identity_size);
  memset (psk, 0, 32);
  return 32;
}

static void
queue_rx (const guint8 *data, gsize size)
{
  for (gsize offset = 0; offset < size; offset += fragment_size)
    g_queue_push_tail (&incoming, g_bytes_new (data + offset, MIN (fragment_size, size - offset)));
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
queue_reply (guint8 command, const guint8 *data, guint16 size)
{
  g_autofree guint8 *protocol = NULL;
  guint32 length;

  goodix_encode_protocol (command, (guint8 *) data, size, TRUE, FALSE, &protocol, &length);
  queue_pack (GOODIX_FLAGS_MSG_PROTOCOL, protocol, length);
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
  if (result == 1 && cancel_settle && test_cancel)
    g_cancellable_cancel (test_cancel);
}

static void
send_frame (void)
{
  guint16 pixels[GOODIX55X4_FRAME_SIZE];
  guint8 raw[GOODIX55X4_RAW_FRAME_SIZE + 4] = {0};
  guint moving_frames = short_swipe ? 2 : 16;
  gboolean empty = calibration_next || (!hold_finger && frame_number > moving_frames + 8);

  if (!calibration_next && idle_frames_before_touch)
    {
      idle_frames_before_touch--;
      empty = TRUE;
    }
  if (calibration_next)
    {
      /* Detect the original bug: a held finger must never become background
       * for a new enrollment stage. */
      g_assert_false (sensor_has_finger);
      calibration_count++;
      calibration_next = FALSE;
    }
  else
    {
      sensor_has_finger = !empty;
    }

  guint stripe = MIN (frame_number, moving_frames);
  for (guint y = 0; y < GOODIX55X4_HEIGHT; y++)
    for (guint x = 0; x < GOODIX55X4_WIDTH; x++)
      {
        guint16 value = 2600;
        if (!empty)
          {
            /* Public repository print fixture, repeated vertically to supply
             * enough distinct stripes. This tests plumbing, not recognition
             * accuracy or a realistic swipe velocity. */
            guint px = CLAMP ((gint) y - 4, 0, GOODIX55X4_SWIPE_FRAME_W - 1);
            guint py = (stripe * GOODIX55X4_SWIPE_FRAME_H + x) % 384;
            const guint8 *row = cairo_image_surface_get_data (fingerprint) +
                                py * cairo_image_surface_get_stride (fingerprint);
            guint32 pixel;
            memcpy (&pixel, row + px * sizeof (pixel), sizeof (pixel));
            value = 2100 - (pixel & 0xff) * 4;
          }
        pixels[y * GOODIX55X4_WIDTH + x] = value;
      }
  for (guint i = 0, j = 0; i < GOODIX55X4_FRAME_SIZE; i += 4, j += 6)
    {
      raw[j] = (pixels[i] >> 8) | ((pixels[i + 1] & 15) << 4);
      raw[j + 1] = pixels[i];
      raw[j + 2] = pixels[i + 2];
      raw[j + 3] = pixels[i + 1] >> 4;
      raw[j + 4] = pixels[i + 3] >> 4;
      raw[j + 5] = (pixels[i + 2] >> 8) | ((pixels[i + 3] & 15) << 4);
    }
  if (!empty)
    {
      if (frame_number > moving_frames)
        held_frames++;
      frame_number++;
    }
  if (hold_finger && held_frames >= 5 && test_cancel)
    g_cancellable_cancel (test_cancel);
  g_assert_cmpint (SSL_write (sensor, raw, 5000), ==, 5000);
  g_assert_cmpint (SSL_write (sensor, raw + 5000, sizeof (raw) - 5000), ==, sizeof (raw) - 5000);
  guint8 encrypted[20000] = {0};
  int length = BIO_read (SSL_get_wbio (sensor), encrypted + 9, sizeof (encrypted) - 9);
  g_assert_cmpint (length, >, 0);
  queue_pack (GOODIX_FLAGS_TLS_DATA, encrypted, length + 9);
}

static gboolean
delayed_fdt_up_reply (gpointer unused)
{
  queue_reply (GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, (guint8[]){0, 0}, 2);
  return G_SOURCE_REMOVE;
}

static void
sensor_write (FpDevice *device, const guint8 *data, gsize size)
{
  g_byte_array_append (outgoing, data, size);
  GoodixPacket packet;
  gint parsed = goodix_packet_peek (outgoing->data, outgoing->len, &packet);
  g_assert_cmpint (parsed, >=, 0);
  if (!parsed)
    return;
  if (packet.flags == GOODIX_FLAGS_TLS)
    {
      sensor_handshake (packet.payload, packet.length);
    }
  else
    {
      guint8 command = packet.payload[0];
      guint8 ack[] = {command, 1};
      if (fail_sleep && command == GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE)
        ack[1] = 0;
      if (command == cancel_on_command && test_cancel)
        {
          g_cancellable_cancel (test_cancel);
          if (stop_after_ack)
            {
              queue_reply (GOODIX_CMD_ACK, ack, sizeof (ack));
              goto out;
            }
        }
      if (command != GOODIX_CMD_NOP)
        queue_reply (GOODIX_CMD_ACK, ack, sizeof (ack));
      switch (command)
        {
        case GOODIX_CMD_NOP:
        case GOODIX_CMD_ENABLE_CHIP:
        case GOODIX_CMD_MCU_SWITCH_TO_IDLE_MODE:
        case GOODIX_CMD_NAV_0:
        case GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED:
          break;

        case GOODIX_CMD_FIRMWARE_VERSION:
          {
            const guint8 fw[] = "GF3206_RTSEC_APP_10052";
            queue_reply (command, fw, sizeof (fw));
            break;
          }

        case GOODIX_CMD_PRESET_PSK_READ:
          {
            guint8 psk[41] = {0, 7, 0, 2, 0xbb, 32};
            memcpy (psk + 9, goodix_55x4_psk_0, 32);
            queue_reply (command, psk, sizeof (psk));
            break;
          }

        case GOODIX_CMD_RESET:
          queue_reply (command, (guint8[]){1, 0, 4}, 3);
          break;

        case GOODIX_CMD_UPLOAD_CONFIG_MCU:
          queue_reply (command, (guint8[]){1, 0}, 2);
          break;

        case GOODIX_CMD_REQUEST_TLS_CONNECTION:
          SSL_free (sensor);
          sensor = SSL_new (sensor_ctx);
          SSL_set_bio (sensor, BIO_new (BIO_s_mem ()), BIO_new (BIO_s_mem ()));
          BIO_set_mem_eof_return (SSL_get_rbio (sensor), -1);
          BIO_set_mem_eof_return (SSL_get_wbio (sensor), -1);
          SSL_set_connect_state (sensor);
          sensor_handshake (NULL, 0);
          break;

        case GOODIX_CMD_QUERY_MCU_STATE:
          queue_reply (command, (guint8[]){1, 2, 0x30, 0}, 4);
          break;

        case GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE:
          mode_commands++;
          if (invalid_baseline)
            queue_reply (command, (guint8[]){0, 0, 0, 0, 0x60}, 5);
          else if (!drop_mode_reply)
            queue_reply (command, (guint8[]){0, 0, 0, 0, 0x60, 1, 0x62, 1}, 8);
          break;

        case GOODIX_CMD_MCU_SWITCH_TO_FDT_UP:
          calibration_next = TRUE;
          if (slow_fdt_up_count)
            {
              slow_fdt_up_count--;
              g_timeout_add (slow_fdt_up_ms, delayed_fdt_up_reply, NULL);
            }
          else
            {
              queue_reply (command, (guint8[]){0, 0}, 2);
            }
          break;

        case GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN:
          g_assert_false (sensor_has_finger);
          swipe_number++;
          frame_number = 0;
          queue_reply (command, (guint8[]){0, 0}, 2);
          break;

        case GOODIX_CMD_SET_LED:
          led_commands++;
          /* This unsolicited response occurs on the actual notebook. */
          queue_reply (command, (guint8[]){1, 0}, 2);
          break;

        case GOODIX_CMD_MCU_GET_IMAGE:
          if (lost_tls || (lost_tls_swipe && !calibration_next))
            queue_reply (GOODIX_CMD_REQUEST_TLS_CONNECTION, (guint8[]){0, 0}, 2);
          else
            send_frame ();
          break;

        case GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE:
          sleep_commands++;
          break;

        case GOODIX_CMD_MCU_SWITCH_TO_SLEEP_MODE_REALTEK:
          sleep_commands++;
          queue_reply (command, (guint8[]){1, 0}, 2);
          sensor_has_finger = FALSE;
          break;

        default:
          g_error ("Unexpected USB command 0x%02x", command);
        }
    }
out:
  g_byte_array_set_size (outgoing, 0);
}

static gboolean
watchdog (gpointer unused)
{
  g_error ("Public Goodix operation exceeded its test deadline");
  return G_SOURCE_REMOVE;
}

static FpDevice *
new_device (void)
{
  FpDeviceClass *class = g_type_class_ref (fpi_device_goodixtls55x4_get_type ());

  /* USB calls are mocked at the boundary. Virtual type only skips the core's
   * real USB open; all image-device activation/completion callbacks remain. */
  class->type = FP_DEVICE_TYPE_VIRTUAL;
  FpDevice *device = g_object_new (fpi_device_goodixtls55x4_get_type (), NULL);
  g_type_class_unref (class);
  return device;
}

static void
setup_peer (guint fragment)
{
  fragment_size = fragment;
  calibration_count = swipe_number = frame_number = held_frames = 0;
  sleep_commands = led_commands = mode_commands = usb_resets = 0;
  slow_fdt_up_ms = slow_fdt_up_count = idle_frames_before_touch = 0;
  calibration_next = short_swipe = hold_finger = sensor_has_finger = FALSE;
  stop_after_ack = drop_mode_reply = fail_sleep = cancel_settle = lost_tls = invalid_baseline = FALSE;
  lost_tls_swipe = FALSE;
  cancel_on_command = 0xff;
  test_cancel = NULL;
  sent_data = g_byte_array_new ();
  outgoing = g_byte_array_new ();
  observe_write = sensor_write;
  sensor_ctx = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (sensor_ctx);
  SSL_CTX_set_security_level (sensor_ctx, 0);
  g_assert_cmpint (SSL_CTX_set_cipher_list (sensor_ctx, "PSK-AES128-CBC-SHA256"), ==, 1);
  SSL_CTX_set_min_proto_version (sensor_ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (sensor_ctx, TLS1_2_VERSION);
  SSL_CTX_set_psk_client_callback (sensor_ctx, sensor_psk);
}

static void
teardown_peer (void)
{
  drain_writes ();
  g_assert_null (pending_read);
  /* Closing may cancel the read after the final reply but before the rest
   * of its USB padding arrives. No protocol bytes may remain. */
  while (!g_queue_is_empty (&incoming))
    {
      g_autoptr(GBytes) bytes = g_queue_pop_head (&incoming);
      gsize size;
      const guint8 *padding = g_bytes_get_data (bytes, &size);
      for (gsize i = 0; i < size; i++)
        g_assert_cmpuint (padding[i], ==, 0);
    }
  g_assert_cmpuint (outgoing->len, ==, 0);
  SSL_free (sensor);
  sensor = NULL;
  SSL_CTX_free (sensor_ctx);
  g_clear_pointer (&sent_data, g_byte_array_unref);
  g_clear_pointer (&outgoing, g_byte_array_unref);
  observe_write = NULL;
  test_cancel = NULL;
}

static void
assert_inactive (FpDevice *device)
{
  FpiImageDeviceState state;

  g_object_get (device, "fpi-image-device-state", &state, NULL);
  g_assert_cmpint (state, ==, FPI_IMAGE_DEVICE_STATE_INACTIVE);
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (device);
  g_assert_null (self->activation_ssm);
  g_assert_null (self->scan_ssm);
  g_assert_null (self->delay);
}

static void
test_capture (gconstpointer fragment)
{
  setup_peer (GPOINTER_TO_UINT (fragment));
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (12, watchdog, NULL);
  for (guint cycle = 0; cycle < 2; cycle++)
    {
      g_assert_true (fp_device_open_sync (device, NULL, &error));
      g_assert_no_error (error);
      g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
      g_assert_no_error (error);
      g_assert_nonnull (image);
      g_assert_cmpuint (fp_image_get_width (image), ==, 168);
      g_assert_cmpuint (fp_image_get_height (image), >=, 12 * 48);
      g_assert_false (sensor_has_finger);
      assert_inactive (device);
      g_assert_true (fp_device_close_sync (device, NULL, &error));
      g_assert_no_error (error);
      drain_writes ();
    }
  g_assert_cmpuint (calibration_count, ==, 2);
  g_assert_cmpuint (led_commands, ==, 2);
  g_assert_cmpuint (sleep_commands, ==, 4);
  g_assert_cmpuint (held_frames, >=, 8);
  g_source_remove (guard);
  teardown_peer ();
}

static guint enroll_progress;

static void
enrolled_stage (FpDevice *device, gint completed, FpPrint *print, gpointer unused, GError *error)
{
  g_assert_no_error (error);
  g_assert_cmpuint (completed, ==, ++enroll_progress);
  g_assert_cmpuint (swipe_number, ==, completed);
}

static void
test_enroll (void)
{
  setup_peer (65536);
  enroll_progress = 0;
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (20, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) print = fp_device_enroll_sync (device, fp_print_new (device), NULL,
                                                    enrolled_stage, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  g_assert_cmpuint (enroll_progress, ==, 6);
  g_assert_cmpuint (calibration_count, ==, 1);
  g_assert_cmpuint (led_commands, ==, 6);
  assert_inactive (device);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  /* Recreate both the device and serialized print: a match must not depend
   * on an old TLS connection or enrollment-only in-memory state. */
  g_autofree guint8 *serialized = NULL;
  gsize length;
  g_assert_true (fp_print_serialize (print, &serialized, &length, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) restored = fp_print_deserialize (serialized, length, &error);
  g_assert_no_error (error);
  g_clear_object (&device);
  device = new_device ();
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  gboolean matched = FALSE;
  g_assert_true (fp_device_verify_sync (device, restored, NULL, NULL, NULL, &matched, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (matched);
  g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (gallery, g_object_ref (restored));
  g_autoptr(FpPrint) identified = NULL;
  g_assert_true (fp_device_identify_sync (device, gallery, NULL, NULL, NULL, &identified, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (fp_print_equal (identified, restored));
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

static gboolean
cancel_later (gpointer unused)
{
  g_cancellable_cancel (test_cancel);
  return G_SOURCE_REMOVE;
}

static void
cancel_at_state (GObject *object, GParamSpec *pspec, gpointer state)
{
  FpiImageDeviceState current;

  g_object_get (object, "fpi-image-device-state", &current, NULL);
  if (current == GPOINTER_TO_INT (state))
    g_cancellable_cancel (test_cancel);
}

static void
test_cancel_reactivate (gconstpointer phase)
{
  setup_peer (65536);
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  g_autoptr(GCancellable) cancel = g_cancellable_new ();
  test_cancel = cancel;
  guint guard = g_timeout_add_seconds (12, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  guint which = GPOINTER_TO_UINT (phase);
  gulong handler = 0;
  if (which < 256)
    {
      cancel_on_command = which;
      stop_after_ack = TRUE;
    }
  else if (which == 256)
    {
      cancel_settle = TRUE;
    }
  else if (which == 257)
    {
      fail_write = TRUE;
      g_timeout_add (25, cancel_later, NULL); /* activation retry backoff */
    }
  else if (which == 260)
    {
      hold_finger = TRUE;
    }
  else
    {
      handler = g_signal_connect (device, "notify::fpi-image-device-state", G_CALLBACK (cancel_at_state),
                                  GINT_TO_POINTER (which == 258 ? FPI_IMAGE_DEVICE_STATE_CAPTURE :
                                                   FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF));
    }
  gint64 start = g_get_monotonic_time ();
  g_assert_null (fp_device_capture_sync (device, TRUE, cancel, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error (&error);
  if (which <= 257)
    g_assert_cmpint (g_get_monotonic_time () - start, <, 2 * G_TIME_SPAN_SECOND);
  assert_inactive (device);
  if (handler)
    g_signal_handler_disconnect (device, handler);
  cancel_on_command = 0xff;
  cancel_settle = FALSE;
  hold_finger = FALSE;
  stop_after_ack = FALSE;
  test_cancel = NULL;

  /* Reuse the same open device immediately. Late packets and cancelled
   * callbacks from the first attempt are still allowed to arrive. */
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  assert_inactive (device);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

static void
test_bad_reply (gconstpointer fault)
{
  setup_peer (65536);
  lost_tls = GPOINTER_TO_UINT (fault) == 0;
  invalid_baseline = GPOINTER_TO_UINT (fault) == 1;
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (8, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  /* Both faults persist, so the single initialization retry fails too. */
  g_assert_null (fp_device_capture_sync (device, TRUE, NULL, &error));
  GIOErrorEnum expected = lost_tls ? G_IO_ERROR_CONNECTION_CLOSED : G_IO_ERROR_INVALID_DATA;
  g_assert_error (error, G_IO_ERROR, expected);
  g_clear_error (&error);
  g_assert_cmpuint (usb_resets, ==, 0); /* The MCU answered; no port reset. */
  assert_inactive (device);
  lost_tls = invalid_baseline = FALSE;
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

static void
test_short_swipe (void)
{
  setup_peer (65536);
  short_swipe = TRUE;
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (8, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_null (fp_device_capture_sync (device, TRUE, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_TOO_SHORT);
  g_clear_error (&error);
  assert_inactive (device);
  short_swipe = FALSE;
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

static void
test_mode_timeout (void)
{
  setup_peer (65536);
  drop_mode_reply = TRUE;
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  /* Two reply timeouts, one port reset with its two-second settle, and a
   * final timed-out initialization. */
  guint guard = g_timeout_add_seconds (15, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_null (fp_device_capture_sync (device, TRUE, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_assert_nonnull (strstr (error->message, "configure finger detection"));
  g_clear_error (&error);
  assert_inactive (device);
  g_assert_cmpuint (usb_resets, ==, 1);
  g_assert_cmpuint (mode_commands, ==, 3);
  g_assert_cmpuint (sleep_commands, ==, 0); /* Failed before activation completed. */
  /* The reset budget is per open: a second attempt in this open does not
   * reset again, a new open may. */
  g_assert_null (fp_device_capture_sync (device, TRUE, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&error);
  g_assert_cmpuint (usb_resets, ==, 1);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  drop_mode_reply = FALSE;
  g_source_remove (guard);
  teardown_peer ();
}

static void
test_sleep_failure_keeps_result (void)
{
  setup_peer (65536);
  fail_sleep = TRUE;
  short_swipe = TRUE;
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (8, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  /* The sensor rejects sleep after a short swipe. The caller must still see
   * the retry; previously the sleep error replaced it and ended PAM. */
  g_assert_null (fp_device_capture_sync (device, TRUE, NULL, &error));
  g_assert_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_TOO_SHORT);
  g_clear_error (&error);
  g_assert_cmpuint (sleep_commands, ==, 2); /* MCU sleep still attempted after sensor sleep failed. */
  assert_inactive (device);
  /* A good swipe is returned even though sleep fails again. */
  short_swipe = FALSE;
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  assert_inactive (device);
  fail_sleep = FALSE;
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

static FpPrint *
enroll_print (FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  FpPrint *print = fp_device_enroll_sync (device, fp_print_new (device), NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  return print;
}

static void
verify_expect (FpDevice *device, FpPrint *print, GQuark domain, gint code)
{
  g_autoptr(GError) error = NULL;
  gboolean matched = FALSE;
  gboolean ok = fp_device_verify_sync (device, print, NULL, NULL, NULL, &matched, NULL, &error);
  if (!domain)
    {
      g_assert_no_error (error);
      g_assert_true (ok);
      g_assert_true (matched);
    }
  else
    {
      g_assert_false (ok);
      g_assert_error (error, domain, code);
    }
  assert_inactive (device);
}

static void
test_verify_transient_retry (void)
{
  setup_peer (65536);
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (30, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpPrint) print = enroll_print (device);

  /* The sensor drops its TLS session mid-swipe. fprintd restarts a verify
   * that ends in a retry, so the user just swipes again. */
  lost_tls_swipe = TRUE;
  verify_expect (device, print, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
  verify_expect (device, print, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
  /* A persistent fault still ends the attempt with the real error. */
  verify_expect (device, print, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
  /* Recovery: the next swipe matches and restores the retry budget. */
  lost_tls_swipe = FALSE;
  verify_expect (device, print, 0, 0);
  lost_tls_swipe = TRUE;
  verify_expect (device, print, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL);
  lost_tls_swipe = FALSE;
  verify_expect (device, print, 0, 0);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

static void
test_rebaseline (gconstpointer always_slow)
{
  setup_peer (65536);
  /* The user's finger still rests on the sensor while finger detection is
   * configured, so the finger-up answer is late. */
  slow_fdt_up_ms = 2 * GOODIX_REBASELINE_AFTER_MS;
  slow_fdt_up_count = always_slow ? 2 : 1;
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (8, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  /* Detection is configured again on the empty sensor, exactly once. */
  g_assert_cmpuint (mode_commands, ==, 2);
  g_assert_cmpuint (calibration_count, ==, 2);
  g_assert_cmpuint (swipe_number, ==, 1);
  assert_inactive (device);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

static void
test_idle_rearm (void)
{
  setup_peer (65536);
  /* Finger detection fires with nothing touching the sensor for longer than
   * the idle budget; the driver returns to the finger wait, then a real
   * swipe arrives. */
  idle_frames_before_touch = GOODIX_IDLE_REARM_FRAMES + 10;
  g_autoptr(FpDevice) device = new_device ();
  g_autoptr(GError) error = NULL;
  guint guard = g_timeout_add_seconds (10, watchdog, NULL);
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_autoptr(FpImage) image = fp_device_capture_sync (device, TRUE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (image);
  g_assert_cmpuint (swipe_number, ==, 2); /* finger detection armed twice */
  g_assert_cmpuint (led_commands, ==, 2);
  g_assert_cmpuint (calibration_count, ==, 1);
  assert_inactive (device);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_source_remove (guard);
  teardown_peer ();
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_autofree gchar *path = g_build_filename (g_getenv ("MESON_SOURCE_ROOT"), "examples", "prints", "whorl.png", NULL);
  cairo_surface_t *source = cairo_image_surface_create_from_png (path);
  g_assert_cmpint (cairo_surface_status (source), ==, CAIRO_STATUS_SUCCESS);
  fingerprint = cairo_image_surface_create (CAIRO_FORMAT_RGB24, 168, 384);
  cairo_t *cr = cairo_create (fingerprint);
  cairo_scale (cr, 168.0 / cairo_image_surface_get_width (source), 384.0 / cairo_image_surface_get_height (source));
  cairo_set_source_surface (cr, source, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);
  cairo_surface_destroy (source);
  cairo_surface_flush (fingerprint);
  g_test_add_data_func ("/goodixtls/public/capture-reopen", GUINT_TO_POINTER (65536), test_capture);
  g_test_add_data_func ("/goodixtls/public/capture-fragmented", GUINT_TO_POINTER (7), test_capture);
  g_test_add_func ("/goodixtls/public/enroll-held-finger", test_enroll);
  g_test_add_func ("/goodixtls/public/short-swipe-retry", test_short_swipe);
  g_test_add_func ("/goodixtls/public/mode-reply-timeout", test_mode_timeout);
  g_test_add_data_func ("/goodixtls/public/cancel-firmware", GUINT_TO_POINTER (GOODIX_CMD_FIRMWARE_VERSION), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/cancel-fdt-up", GUINT_TO_POINTER (GOODIX_CMD_MCU_SWITCH_TO_FDT_UP), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/cancel-fdt-down", GUINT_TO_POINTER (GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/cancel-tls-settle", GUINT_TO_POINTER (256), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/cancel-retry-delay", GUINT_TO_POINTER (257), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/cancel-frame-delay", GUINT_TO_POINTER (258), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/cancel-minutiae", GUINT_TO_POINTER (259), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/cancel-held-finger", GUINT_TO_POINTER (260), test_cancel_reactivate);
  g_test_add_data_func ("/goodixtls/public/lost-tls", GUINT_TO_POINTER (0), test_bad_reply);
  g_test_add_data_func ("/goodixtls/public/invalid-fdt-baseline", GUINT_TO_POINTER (1), test_bad_reply);
  g_test_add_func ("/goodixtls/public/sleep-failure-keeps-result", test_sleep_failure_keeps_result);
  g_test_add_func ("/goodixtls/public/verify-transient-retry", test_verify_transient_retry);
  g_test_add_data_func ("/goodixtls/public/rebaseline-held-finger", NULL, test_rebaseline);
  g_test_add_data_func ("/goodixtls/public/rebaseline-once", GINT_TO_POINTER (1), test_rebaseline);
  g_test_add_func ("/goodixtls/public/idle-rearm", test_idle_rearm);
  int result = g_test_run ();
  cairo_surface_destroy (fingerprint);
  return result;
}
