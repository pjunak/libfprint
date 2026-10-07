// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>

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

#define FP_COMPONENT "goodixtls55x4"

#include "drivers_api.h"
#include "goodix.h"
#include "goodix55x4.h"
#include "goodix_proto.h"
#include "goodix-profiles.h"
#include "goodix-image.h"

#include "goodix-swipe.h"
#include <string.h>

/* Poll at most 25 frames/s; waiting for a finger must not busy-loop. */
#define CAPTURE_INTERVAL_MS 40

/* Capture errors that a fresh activation can clear are reported as retries
 * during verify/identify, at most this often per open (one PAM attempt). */
#define GOODIX_TRANSIENT_RETRIES 2

/* An empty sensor answers finger-up promptly. A slower answer means a finger
 * was resting while finger detection measured its baseline. */
#define GOODIX_REBASELINE_AFTER_MS 300

/* Finger detection that fires without contact returns to the low-power wait
 * after two seconds of empty frames instead of streaming until cancelled. */
#define GOODIX_IDLE_REARM_FRAMES 50

struct _FpiDeviceGoodixTls55X4 {
  FpiDeviceGoodixTls parent;
  const GoodixProfile *profile;
  FpiSsm *activation_ssm;
  FpiSsm *scan_ssm;
  GSource *delay;
  gboolean activating;
  gboolean deactivating;
  guint activation_retries;
  guint transient_retries;
  gboolean usb_reset_done;

  gboolean calibrated;
  gboolean rebaselined;
  gint64 wait_empty_started;
  Goodix55X4Pix empty_img[GOODIX55X4_FRAME_SIZE];
  gint empty_mean;
  guint8 fdt_down_dyn[26];
  guint8 fdt_down_len;
  GoodixSwipe swipe;
  guint release_frames;
  guint idle_frames;
  gboolean image_reported;
  gint64 frame_started;
  FpImage *pending_image;
  FpDeviceRetry pending_retry;
  GError *sleep_error;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls55X4, fpi_device_goodixtls55x4, FPI,
                      DEVICE_GOODIXTLS55X4, FpiDeviceGoodixTls);
G_DEFINE_TYPE (FpiDeviceGoodixTls55X4, fpi_device_goodixtls55x4,
               FPI_TYPE_DEVICE_GOODIXTLS);

static void
goodix55x4_clear_capture (FpiDeviceGoodixTls55X4 *self)
{
  goodix_swipe_clear (&self->swipe);
  g_clear_object (&self->pending_image);
  self->calibrated = FALSE;
  self->rebaselined = FALSE;
  self->wait_empty_started = 0;
  self->empty_mean = 0;
  self->fdt_down_len = 0;
}

enum activate_states {
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_RESET,
  ACTIVATE_SET_MCU_IDLE,
  ACTIVATE_SET_MCU_CONFIG,
  ACTIVATE_NUM_STATES,
};

static void
check_none (FpDevice *dev, gpointer ssm, GError *error)
{
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else
    fpi_ssm_next_state (ssm);
}

static void
check_firmware_version (FpDevice *dev, gchar *firmware, gpointer ssm, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  guint16 pid = g_usb_device_get_pid (fpi_device_get_usb_device (dev));
  fp_dbg ("Device firmware: \"%s\"", firmware);
  self->profile = goodix_profile_lookup (pid, firmware);
  if (!self->profile)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                           "Unsupported Goodix firmware '%s' for USB device 27c6:%04x",
                                           firmware, pid));
      return;
    }
  g_assert (self->profile->width == GOODIX55X4_WIDTH && self->profile->height == GOODIX55X4_HEIGHT &&
            self->profile->crop == GOODIX55X4_CROP);
  goodix_set_tls_settle_time (dev, self->profile->tls_settle_ms);
  fpi_ssm_next_state (ssm);
}

static void
check_reset (FpDevice *dev, gboolean success, guint16 number, gpointer ssm, GError *error)
{
  if (!error && !success)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to reset device");
  if (!error && number != FPI_DEVICE_GOODIXTLS55X4 (dev)->profile->reset_number)
    error = g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid device reset number: %u", number);
  check_none (dev, ssm, error);
}

static void
check_preset_psk_read (FpDevice *dev, gboolean success, guint32 flags,
                       guint8 *psk, guint16 length, gpointer ssm, GError *error)
{
  /* A well-formed reply with another key is a provisioning problem (for
   * example after Windows re-paired the sensor). Retrying cannot fix it. */
  if (!error && !success)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to read PSK from device");
  if (!error && flags != GOODIX_55X4_PSK_FLAGS)
    error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Invalid device PSK flags: 0x%08x", flags);
  if (!error && length != sizeof (goodix_55x4_psk_0))
    error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Invalid device PSK hash length: %u", length);
  if (!error && memcmp (psk, goodix_55x4_psk_0, sizeof (goodix_55x4_psk_0)))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                 "Invalid device PSK: does not match the supported 55a2 profile");
  check_none (dev, ssm, error);
}

static void
check_config_upload (FpDevice *dev, gboolean success, gpointer ssm, GError *error)
{
  if (!error && !success)
    error = g_error_new_literal (FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO, "Failed to upload MCU config");
  check_none (dev, ssm, error);
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  if (fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                   "Fingerprint activation cancelled"));
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_READ_AND_NOP:
      goodix_start_read_loop (dev);
      goodix_send_nop (dev, check_none, ssm);
      break;
    case ACTIVATE_ENABLE_CHIP:
      goodix_send_enable_chip (dev, TRUE, check_none, ssm);
      break;
    case ACTIVATE_NOP:
      goodix_send_nop (dev, check_none, ssm);
      break;
    case ACTIVATE_CHECK_FW_VER:
      goodix_send_firmware_version (dev, check_firmware_version, ssm);
      break;
    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read (dev, GOODIX_55X4_PSK_FLAGS, sizeof (goodix_55x4_psk_0),
                                  check_preset_psk_read, ssm);
      break;
    case ACTIVATE_RESET:
      goodix_send_reset (dev, TRUE, 20, check_reset, ssm);
      break;
    case ACTIVATE_SET_MCU_IDLE:
      goodix_send_mcu_switch_to_idle_mode (dev, 20, check_none, ssm);
      break;
    case ACTIVATE_SET_MCU_CONFIG:
      goodix_send_upload_config_mcu (dev, (guint8 *) self->profile->config, self->profile->config_length,
                                     NULL, check_config_upload, ssm);
      break;
    }
}

static void activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error);
static void prepare_capture (FpDevice *dev);

static void
retry_activation (FpDevice *dev, gpointer unused)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  self->delay = NULL;
  self->activation_ssm = fpi_ssm_new (dev, activate_run_state, ACTIVATE_NUM_STATES);
  fpi_ssm_start (self->activation_ssm, activate_complete);
}

static gboolean
error_is_timeout (const GError *error)
{
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
         g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT);
}

/* Timeouts, stale or corrupt packets from a cancelled session and a lost TLS
 * session can clear on a fresh initialization. Unsupported firmware or keys
 * cannot. */
static gboolean
activation_error_is_transient (const GError *error)
{
  return error_is_timeout (error) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED);
}

static void tls_activation_complete (FpDevice *dev, gpointer user_data, GError *error);

/* Settle time after a port reset, as used by the original opt-in reset. */
#define GOODIX_USB_RESET_SETTLE_MS 2000
/* Wait at most one second for the cancelled IN transfer to call back. */
#define GOODIX_USB_RESET_POLL_MS 50
#define GOODIX_USB_RESET_POLLS 20

static void
reset_usb_when_idle (FpDevice *dev, gpointer polls)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  GError *error = NULL;
  guint count = GPOINTER_TO_UINT (polls);

  self->delay = NULL;
  /* Never reset the port underneath an outstanding libusb transfer. */
  if (goodix_read_pending (dev) && count < GOODIX_USB_RESET_POLLS)
    {
      self->delay = fpi_device_add_timeout (dev, GOODIX_USB_RESET_POLL_MS, reset_usb_when_idle,
                                           GUINT_TO_POINTER (count + 1), NULL);
      return;
    }
  if (goodix_read_pending (dev))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                 "Cancelled USB read did not finish before the device reset");
  else if (!goodix_reset_usb (dev, &error) && !error)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Fingerprint sensor USB reset failed");

  if (error)
    {
      tls_activation_complete (dev, NULL, error);
      return;
    }
  self->delay = fpi_device_add_timeout (dev, GOODIX_USB_RESET_SETTLE_MS, retry_activation, NULL, NULL);
}

static void
tls_activation_complete (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  gboolean cancelled = fpi_device_action_is_cancelled (dev);
  if (!error && cancelled)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Fingerprint activation cancelled");

  if (!error)
    {
      /* Do not advertise AWAIT_FINGER_ON while the reader still needs an
       * empty calibration image. Initialization must finish first. */
      prepare_capture (dev);
      return;
    }

  g_clear_pointer (&self->delay, g_source_destroy);
  goodix_cancel_receive (dev);
  goodix_shutdown_tls (dev, NULL);
  if (!cancelled && activation_error_is_transient (error))
    {
      if (self->activation_retries++ == 0)
        {
          g_message ("Fingerprint sensor initialization failed (%s); retrying once", error->message);
          g_clear_error (&error);
          goodix_reset_state (dev);
          self->delay = fpi_device_add_timeout (dev, 250, retry_activation, NULL, NULL);
          return;
        }
      /* Two timed-out initializations mean the MCU stopped answering. A
       * port reset is the only recovery short of replugging; do it once
       * per open so a dead sensor still fails within one PAM attempt. */
      if (error_is_timeout (error) && !self->usb_reset_done)
        {
          self->usb_reset_done = TRUE;
          g_message ("Fingerprint sensor stopped responding (%s); resetting its USB port once",
                     error->message);
          g_clear_error (&error);
          goodix_reset_state (dev);
          reset_usb_when_idle (dev, GUINT_TO_POINTER (0));
          return;
        }
    }
  if (!cancelled)
    g_message ("Fingerprint sensor initialization failed: %s", error->message);
  self->activating = FALSE;
  fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FPI_DEVICE_GOODIXTLS55X4 (dev)->activation_ssm = NULL;
  if (error)
    tls_activation_complete (dev, NULL, error);
  else
    goodix_tls (dev, tls_activation_complete, NULL);
}

/* Calibration uses the historical 55a2 mode/up/nav sequence. Its FDT-up
 * reply is the prerequisite for reading an empty image. Never invent a new
 * baseline while a finger is still resting on the sensor. */
static const guint8 fdt_mode[] = {
  0x0d, 0x01, 0x80, 0x12, 0x80, 0xaf, 0x80, 0x9a, 0x80, 0x87, 0x80, 0x12,
  0x80, 0xa8, 0x80, 0x95, 0x80, 0x81, 0x80, 0x12, 0x80, 0xa7, 0x80, 0x98, 0x80, 0x84
};
static const guint8 fdt_up[] = {
  0x0e, 0x01, 0x80, 0x92, 0x80, 0x9d, 0x80, 0x93, 0x80, 0x92, 0x80, 0x97,
  0x80, 0x9e, 0x80, 0xa0, 0x80, 0x8e, 0x80, 0xab, 0x80, 0xa5, 0x80, 0xb0, 0x80, 0x12
};

enum scan_states {
  SCAN_QUERY_MCU,
  SCAN_FDT_MODE,
  SCAN_WAIT_EMPTY,
  SCAN_NAV,
  SCAN_CALIBRATE,
  SCAN_WAIT_FINGER,
  SCAN_SET_LED,
  SCAN_READ,
  SCAN_REPORT,
  SCAN_WAIT_RELEASE,
  SCAN_DONE,
  SCAN_NUM_STATES,
};

static const char *scan_names[] = {
  "query MCU", "configure finger detection", "wait for empty sensor", "navigation",
  "calibration", "wait for finger", "capture setup", "read swipe", "report swipe", "wait for finger release", "finish"
};

static void
check_none_cmd (FpDevice *dev, guint8 *data, guint16 length, gpointer ssm, GError *error)
{
  check_none (dev, ssm, error);
}

static void
fdt_mode_base_cb (FpDevice *dev, guint8 *data, guint16 length, gpointer ssm, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  /* Four header bytes followed by 1..12 little-endian zone baselines.
   * Truncating or replacing malformed data with another sensor's constants
   * silently arms the wrong detection threshold. */
  if (length < 6 || length > 28 || length % 2)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                           "Invalid finger-detection baseline length: %u", length));
      return;
    }
  guint zones = (length - 4) / 2;
  self->fdt_down_dyn[0] = 0x0c;
  self->fdt_down_dyn[1] = 0x01;
  for (guint zone = 0; zone < zones; zone++)
    {
      guint16 base = data[4 + 2 * zone] | (data[5 + 2 * zone] << 8);
      self->fdt_down_dyn[2 + 2 * zone] = 0x80;
      self->fdt_down_dyn[3 + 2 * zone] = (base >> 1) & 0xff;
    }
  self->fdt_down_len = 2 + 2 * zones;
  fpi_ssm_next_state (ssm);
}

static gboolean
decode_frame (const guint8 *data, guint16 length, Goodix55X4Pix *frame, GError **error)
{
  /* Some captures contain the four-byte firmware trailer. It is not pixels. */
  if (length != GOODIX55X4_RAW_FRAME_SIZE && length != GOODIX55X4_RAW_FRAME_SIZE + 4)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid fingerprint frame length: %u (expected %u or %u)",
                   length, GOODIX55X4_RAW_FRAME_SIZE, GOODIX55X4_RAW_FRAME_SIZE + 4);
      return FALSE;
    }
  goodix_image_decode_frame (frame, data);
  return TRUE;
}

static void
wait_empty_done (FpDevice *dev, guint8 *data, guint16 length, gpointer ssm, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  gint64 waited_ms = (g_get_monotonic_time () - self->wait_empty_started) / 1000;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  self->wait_empty_started = 0;
  fp_dbg ("Sensor reported empty after %" G_GINT64_FORMAT " ms", waited_ms);
  if (waited_ms > 3000)
    g_message ("Waited %" G_GINT64_FORMAT " ms for the finger to leave the sensor before calibration",
               waited_ms);
  /* The finger-detection baseline was measured before this wait. If a finger
   * was resting then, the baseline includes it and a later touch might never
   * be detected. Redo the calibration prefix once after this empty image. */
  if (waited_ms > GOODIX_REBASELINE_AFTER_MS && !self->rebaselined)
    {
      fp_dbg ("Finger was present while measuring the detection baseline; measuring again");
      self->rebaselined = TRUE;
      self->calibrated = FALSE;
      self->fdt_down_len = 0;
    }
  fpi_ssm_next_state (ssm);
}

static void
on_scan_empty_img (FpDevice *dev, guint8 *data, guint16 length, gpointer ssm, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  if (error || !decode_frame (data, length, self->empty_img, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  self->empty_mean = goodix_image_swipe_raw_mean (self->empty_img);
  if (self->empty_mean <= 350 || self->empty_mean == 4095)
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                   "Sensor returned an unusable empty calibration image"));
      return;
    }
  if (!self->fdt_down_len)
    {
      /* Restart at the MCU query, the hardware-proven sequence that follows
       * an image read, so finger detection is configured on an empty sensor. */
      fpi_ssm_jump_to_state (ssm, SCAN_QUERY_MCU);
      return;
    }
  self->calibrated = TRUE;
  goodix_swipe_start (&self->swipe, self->empty_mean);
  fp_dbg ("Empty sensor calibrated (mean %d)", self->empty_mean);
  fpi_ssm_next_state (ssm);
}

static void
next_frame (FpDevice *dev, gpointer state)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  self->delay = NULL;
  fpi_ssm_jump_to_state (self->scan_ssm, GPOINTER_TO_INT (state));
}

static void
schedule_frame (FpDevice *dev, gint state)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  gint64 elapsed_ms = (g_get_monotonic_time () - self->frame_started) / 1000;
  g_assert_null (self->delay);
  self->delay = fpi_device_add_timeout (dev, MAX (1, CAPTURE_INTERVAL_MS - elapsed_ms),
                                       next_frame, GINT_TO_POINTER (state), NULL);
}

static void
scan_on_read_img (FpDevice *dev, guint8 *data, guint16 length, gpointer ssm, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE];
  if (error || !decode_frame (data, length, frame, &error))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (fpi_ssm_get_cur_state (ssm) == SCAN_WAIT_RELEASE)
    {
      /* Movement ending and finger removal are separate events. Never tell
       * libfprint the finger is off just because four frames looked alike. */
      if (goodix_swipe_finger_present (&self->swipe, frame))
        {
          if (++self->release_frames > GOODIX_SWIPE_MAX_FRAMES)
            fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                                         "Finger remained on the sensor after the swipe"));
          else
            schedule_frame (dev, SCAN_WAIT_RELEASE);
        }
      else
        {
          self->swipe.present = FALSE;
          fpi_ssm_jump_to_state (ssm, self->pending_image ? SCAN_REPORT : SCAN_DONE);
        }
      return;
    }

  GoodixSwipeResult result = goodix_swipe_feed (&self->swipe, frame, self->empty_img);
  switch (result)
    {
    case GOODIX_SWIPE_WAIT:
      if (++self->idle_frames >= GOODIX_IDLE_REARM_FRAMES)
        {
          fp_dbg ("Finger detection fired without contact; waiting for a finger again");
          fpi_ssm_jump_to_state (ssm, SCAN_WAIT_FINGER);
          return;
        }
      schedule_frame (dev, SCAN_READ);
      return;
    case GOODIX_SWIPE_FINGER_ON:
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), TRUE);
      if (self->deactivating) return;
      /* fall through */
    case GOODIX_SWIPE_CONTINUE:
      schedule_frame (dev, SCAN_READ);
      return;
    case GOODIX_SWIPE_COMPLETE:
      self->pending_image = goodix_swipe_take_image (&self->swipe);
      /* The last enrollment image can cause the core to deactivate as soon
       * as minutiae extraction finishes. Hold the image until the finger is
       * actually off so every stage has the same lifecycle ordering. */
      if (self->swipe.present)
        {
          schedule_frame (dev, SCAN_WAIT_RELEASE);
          return;
        }
      break;
    case GOODIX_SWIPE_TOO_SHORT:
      self->pending_retry = FP_DEVICE_RETRY_TOO_SHORT;
      break;
    case GOODIX_SWIPE_REMOVE_FINGER:
      self->pending_retry = FP_DEVICE_RETRY_REMOVE_FINGER;
      break;
    }
  fpi_ssm_jump_to_state (ssm, SCAN_REPORT);
}

static void
scan_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_QUERY_MCU:
      /* An enrollment can reuse calibration only after actual finger release. */
      if (self->calibrated)
        fpi_ssm_jump_to_state (ssm, SCAN_WAIT_FINGER);
      else
        goodix_send_query_mcu_state (dev, check_none_cmd, ssm);
      break;
    case SCAN_FDT_MODE:
      goodix_send_mcu_switch_to_fdt_mode (dev, (guint8 *) fdt_mode, sizeof (fdt_mode),
                                         NULL, fdt_mode_base_cb, ssm);
      break;
    case SCAN_WAIT_EMPTY:
      fp_dbg ("Waiting for finger removal before calibration");
      self->wait_empty_started = g_get_monotonic_time ();
      goodix_send_mcu_switch_to_fdt_up (dev, (guint8 *) fdt_up, sizeof (fdt_up),
                                       NULL, wait_empty_done, ssm);
      break;
    case SCAN_NAV:
      goodix_send_nav_0 (dev, check_none_cmd, ssm);
      break;
    case SCAN_CALIBRATE:
      goodix_tls_read_image (dev, on_scan_empty_img, ssm);
      break;
    case SCAN_WAIT_FINGER:
      fp_dbg ("Waiting for finger detection");
      self->idle_frames = 0;
      goodix_send_mcu_switch_to_fdt_down (dev, self->fdt_down_dyn, self->fdt_down_len,
                                         NULL, check_none_cmd, ssm);
      break;
    case SCAN_SET_LED:
      /* Retain the single capture-setup command from the known working
       * sequence. Its extra data reply is unsolicited, not an image. */
      goodix_send_set_led (dev, 0, check_none, ssm);
      break;
    case SCAN_READ:
    case SCAN_WAIT_RELEASE:
      self->frame_started = g_get_monotonic_time ();
      goodix_tls_read_image (dev, scan_on_read_img, ssm);
      break;
    case SCAN_REPORT:
      {
        FpImage *image = g_steal_pointer (&self->pending_image);
        FpDeviceRetry retry = self->pending_retry;
        /* Reporting may synchronously deactivate and free this SSM. No
         * command is pending, so deactivation can retire it immediately. */
        if (image)
          {
            /* The core reports this scan's result; a later error must not
             * report a second one. A good image ends the transient streak. */
            self->image_reported = TRUE;
            self->transient_retries = 0;
            fpi_image_device_image_captured (FP_IMAGE_DEVICE (dev), image);
          }
        else
          fpi_image_device_retry_scan (FP_IMAGE_DEVICE (dev), retry);
        if (self->deactivating) return;
        if (self->swipe.present)
          schedule_frame (dev, SCAN_WAIT_RELEASE);
        else
          fpi_ssm_jump_to_state (ssm, SCAN_DONE);
        break;
      }
    case SCAN_DONE:
      /* Finger-off can synchronously start the next enrollment scan. */
      fpi_ssm_mark_completed (ssm);
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), FALSE);
      break;
    }
}

/* Verification and identification restart with a fresh activation after a
 * retry, which renegotiates TLS and recalibrates. Enrollment keeps one
 * activation for all stages, and capture callers want the actual error. */
static gboolean
scan_error_can_retry (FpiDeviceGoodixTls55X4 *self, const GError *error)
{
  FpiDeviceAction action = fpi_device_get_current_action (FP_DEVICE (self));

  if (action != FPI_DEVICE_ACTION_VERIFY && action != FPI_DEVICE_ACTION_IDENTIFY)
    return FALSE;
  /* After an image the core has reported this attempt's result already. */
  if (self->image_reported || self->transient_retries >= GOODIX_TRANSIENT_RETRIES)
    return FALSE;
  return !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
         !g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE) &&
         !g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_REMOVED);
}

static void
scan_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  self->scan_ssm = NULL;
  goodix_swipe_clear (&self->swipe);
  if (error)
    {
      g_clear_object (&self->pending_image);
      if (self->deactivating)
        g_error_free (error);
      else
        {
          g_prefix_error (&error, "Goodix %s: ", scan_names[fpi_ssm_get_cur_state (ssm)]);
          if (scan_error_can_retry (self, error))
            {
              /* A lost TLS session or a dropped frame should cost the user
               * one more swipe, not the whole PAM attempt. */
              self->transient_retries++;
              g_message ("%s; asking for another swipe", error->message);
              g_error_free (error);
              fpi_image_device_retry_scan (FP_IMAGE_DEVICE (dev), FP_DEVICE_RETRY_GENERAL);
            }
          else
            {
              g_message ("%s", error->message);
              fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
            }
        }
    }
}

static void
scan_start (FpiDeviceGoodixTls55X4 *self)
{
  g_assert_null (self->scan_ssm);
  goodix_swipe_start (&self->swipe, self->empty_mean);
  self->release_frames = 0;
  self->idle_frames = 0;
  self->image_reported = FALSE;
  self->scan_ssm = fpi_ssm_new (FP_DEVICE (self), scan_run_state, SCAN_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, scan_complete);
}

static void
prepare_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  self->activation_ssm = NULL;
  if (!error && fpi_device_action_is_cancelled (dev))
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Calibration cancelled");
  if (error)
    {
      /* The finger-up wait has no deadline. Make a sensor that never reports
       * empty visible instead of an unexplained authentication timeout. */
      if (fpi_ssm_get_cur_state (ssm) == SCAN_WAIT_EMPTY && self->wait_empty_started)
        {
          gint64 waited_ms = (g_get_monotonic_time () - self->wait_empty_started) / 1000;
          if (waited_ms > 3000)
            g_message ("Sensor still reported a finger after %" G_GINT64_FORMAT " ms; it never became ready",
                       waited_ms);
        }
      g_prefix_error (&error, "Goodix %s: ", scan_names[fpi_ssm_get_cur_state (ssm)]);
      tls_activation_complete (dev, NULL, error);
      return;
    }
  self->activating = FALSE;
  fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), NULL);
}

static void
prepare_capture (FpDevice *dev)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  /* Run only the preparation prefix; later scans reuse its calibration. */
  self->activation_ssm = fpi_ssm_new_full (dev, scan_run_state,
                                         SCAN_WAIT_FINGER, SCAN_WAIT_FINGER, "prepare capture");
  fpi_ssm_start (self->activation_ssm, prepare_complete);
}

enum sleep_states {
  SLEEP_IDLE,
  SLEEP_SENSOR,
  SLEEP_MCU,
  SLEEP_NUM_STATES,
};

static void
sleep_step (FpDevice *dev, gpointer ssm, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  /* A failed sleep command must not prevent the remaining cleanup commands. */
  if (!self->sleep_error)
    self->sleep_error = error;
  else
    g_clear_error (&error);
  fpi_ssm_next_state (ssm);
}

static void
sleep_mcu_done (FpDevice *dev, gboolean success, gpointer ssm, GError *error)
{
  if (!error && !success)
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "MCU refused sleep");
  sleep_step (dev, ssm, error);
}

static void
sleep_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SLEEP_IDLE:
      /* Stop a pending FDT/image operation in the sensor, not just on the host. */
      goodix_send_mcu_switch_to_idle_mode (dev, 20, sleep_step, ssm);
      break;
    case SLEEP_SENSOR:
      goodix_send_mcu_switch_to_sleep_mode (dev, 20, sleep_step, ssm);
      break;
    case SLEEP_MCU:
      goodix_send_mcu_switch_to_sleep_mode_realtek (dev, 0x6c, sleep_mcu_done, ssm);
      break;
    }
}

static void
sleep_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  goodix_cancel_receive (dev);
  goodix_reset_state (dev);
  goodix_shutdown_tls (dev, NULL);
  goodix55x4_clear_capture (self);
  self->deactivating = FALSE;
  if (!error)
    error = g_steal_pointer (&self->sleep_error);
  g_clear_error (&self->sleep_error);
  /* Every activation reinitializes the sensor from scratch, so a missed sleep
   * ACK needs no recovery here. Reporting it would replace the match, no-match
   * or retry already delivered to the caller and end its PAM attempt. */
  if (error)
    {
      g_message ("Fingerprint sensor did not enter sleep cleanly: %s", error->message);
      g_clear_error (&error);
    }
  fpi_image_device_deactivate_complete (FP_IMAGE_DEVICE (dev), NULL);
}

/* The TLS key shared with the sensor is the public reference key (see
 * goodix55x4.h), so the link cannot tell the real reader from a device that
 * only claims to be one. The real reader is wired to an internal port; a
 * look-alike has to arrive through an external connector, which firmware
 * describes to the kernel as removable. Refuse a reader if its port, or the
 * port of any hub between it and the computer, is removable: a fake hub must
 * not hide a fake reader behind an "unknown" port. Ports the firmware does
 * not describe are accepted, since nothing distinguishes them.
 * GOODIX_ALLOW_REMOVABLE_PORT=1 in fprintd's environment (a root-owned
 * systemd drop-in) accepts a reader that really is connected externally. */
static const gchar *goodix_sysfs_root = "/sys";

/* @ports lists port numbers from the root hub down to the reader. Returns the
 * sysfs name ("bus-port.port") of the first removable link, or NULL. */
static gchar *
goodix_removable_link (const gchar *sysfs_root, guint bus, const guint8 *ports, guint n_ports)
{
  g_autoptr(GString) name = g_string_new (NULL);

  g_string_printf (name, "%u-", bus);
  for (guint i = 0; i < n_ports; i++)
    {
      g_autofree gchar *path = NULL;
      g_autofree gchar *contents = NULL;

      g_string_append_printf (name, i ? ".%u" : "%u", ports[i]);
      path = g_build_filename (sysfs_root, "bus", "usb", "devices", name->str, "removable", NULL);
      if (g_file_get_contents (path, &contents, NULL, NULL) &&
          g_strcmp0 (g_strstrip (contents), "removable") == 0)
        return g_strdup (name->str);
    }
  return NULL;
}

static gboolean
goodix_check_builtin_port (FpDevice *dev, GError **error)
{
  GUsbDevice *usb = fpi_device_get_usb_device (dev);
  g_autoptr(GUsbDevice) node = NULL;
  g_autofree gchar *removable = NULL;
  guint8 ports[8]; /* USB allows at most seven tiers below the root hub */
  guint n_ports = 0;

  if (!usb || g_strcmp0 (g_getenv ("GOODIX_ALLOW_REMOVABLE_PORT"), "1") == 0)
    return TRUE;

  node = g_object_ref (usb);
  for (;;)
    {
      g_autoptr(GUsbDevice) parent = g_usb_device_get_parent (node);
      if (!parent)
        break;
      if (n_ports == G_N_ELEMENTS (ports))
        {
          g_propagate_error (error, fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                              "Goodix reader is nested too deeply in USB hubs"));
          return FALSE;
        }
      ports[n_ports++] = g_usb_device_get_port_number (node);
      g_set_object (&node, parent);
    }
  for (guint i = 0; i < n_ports / 2; i++)
    {
      guint8 port = ports[i];
      ports[i] = ports[n_ports - 1 - i];
      ports[n_ports - 1 - i] = port;
    }

  removable = goodix_removable_link (goodix_sysfs_root, g_usb_device_get_bus (usb), ports, n_ports);
  if (!removable)
    return TRUE;
  g_message ("Refusing a Goodix reader behind removable USB port %s: the built-in reader is "
             "wired to an internal port, so this may be a device impersonating it. Set "
             "GOODIX_ALLOW_REMOVABLE_PORT=1 for fprintd if the reader really is external.", removable);
  g_propagate_error (error, fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                      "Goodix reader on removable USB port %s refused",
                                                      removable));
  return FALSE;
}

static void
dev_init (FpImageDevice *image_dev)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (image_dev);
  GError *error = NULL;
  /* Retry and reset budgets are per open, which is one fprintd claim. */
  self->transient_retries = 0;
  self->usb_reset_done = FALSE;
  if (goodix_check_builtin_port (FP_DEVICE (image_dev), &error))
    goodix_dev_init (FP_DEVICE (image_dev), &error);
  fpi_image_device_open_complete (image_dev, error);
}

/* libfprint closes the USB handle as soon as close completes. A cancelled
 * read still in flight would then never call back: its device reference would
 * leak and the next open would believe a read is pending. Wait for it, at most
 * as long as the port reset does. */
static void
close_when_idle (FpDevice *dev, gpointer polls)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  guint count = GPOINTER_TO_UINT (polls);
  GError *error = NULL;

  self->delay = NULL;
  if (goodix_read_pending (dev) && count < GOODIX_USB_RESET_POLLS)
    {
      self->delay = fpi_device_add_timeout (dev, GOODIX_USB_RESET_POLL_MS, close_when_idle,
                                           GUINT_TO_POINTER (count + 1), NULL);
      return;
    }
  goodix_dev_deinit (dev, &error);
  fpi_image_device_close_complete (FP_IMAGE_DEVICE (dev), error);
}

static void
dev_deinit (FpImageDevice *image_dev)
{
  goodix_cancel_receive (FP_DEVICE (image_dev));
  close_when_idle (FP_DEVICE (image_dev), GUINT_TO_POINTER (0));
}

static void
dev_activate (FpImageDevice *image_dev)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (image_dev);
  self->activation_retries = 0;
  self->activating = TRUE;
  goodix55x4_clear_capture (self);
  retry_activation (FP_DEVICE (self), NULL);
}

static void
dev_change_state (FpImageDevice *image_dev, FpiImageDeviceState state)
{
  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    scan_start (FPI_DEVICE_GOODIXTLS55X4 (image_dev));
}

static void
dev_deactivate (FpImageDevice *image_dev)
{
  FpDevice *dev = FP_DEVICE (image_dev);
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  self->deactivating = TRUE;
  g_clear_pointer (&self->delay, g_source_destroy);
  goodix_cancel_operation (dev);
  if (self->scan_ssm) /* Paused between frames, with no command to cancel. */
    fpi_ssm_mark_failed (self->scan_ssm,
                        g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Scan cancelled"));
  goodix_start_read_loop (dev);
  fpi_ssm_start (fpi_ssm_new (dev, sleep_run_state, SLEEP_NUM_STATES), sleep_complete);
}

static void
dev_cancel (FpDevice *dev)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (dev);
  if (self->activating)
    {
      /* The image-device parent ignores cancellation until activation has
       * finished. Own it here, including retry backoff and TLS settling. */
      g_clear_pointer (&self->delay, g_source_destroy);
      if (!goodix_cancel_operation (dev))
        tls_activation_complete (dev, NULL,
                                 g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                      "Fingerprint activation cancelled"));
    }
  else
    FP_DEVICE_CLASS (fpi_device_goodixtls55x4_parent_class)->cancel (dev);
}

static void
dev_finalize (GObject *object)
{
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4 (object);
  g_clear_pointer (&self->delay, g_source_destroy);
  goodix55x4_clear_capture (self);
  g_clear_error (&self->sleep_error);
  G_OBJECT_CLASS (fpi_device_goodixtls55x4_parent_class)->finalize (object);
}

static void
fpi_device_goodixtls55x4_init (FpiDeviceGoodixTls55X4 *self)
{
}

static void
fpi_device_goodixtls55x4_class_init (FpiDeviceGoodixTls55X4Class *class)
{
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS (class);

  G_OBJECT_CLASS (class)->finalize = dev_finalize;
  dev_class->cancel = dev_cancel;

  gx_class->interface = GOODIX_55X4_INTERFACE;
  gx_class->ep_in = GOODIX_55X4_EP_IN;
  gx_class->ep_out = GOODIX_55X4_EP_OUT;

  dev_class->id = "goodixtls55x4";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 55X4";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 6;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  /* Retain the experimental fork's long-enrollment thermal model. These
   * values are not a measured temperature limit; hardware validation is still
   * required before treating prolonged streaming as thermally safe. */
  dev_class->temp_hot_seconds = 30 * 60;
  dev_class->temp_cold_seconds = 9 * 60;

  /* Historical tuning from a small capture set. Preserve the threshold;
   * representative false-accept/false-reject testing is still required. */
  img_dev_class->bz3_threshold = 24;
  img_dev_class->algorithm = FPI_DEVICE_ALGO_NBIS;
  img_dev_class->img_width = GOODIX55X4_SWIPE_FRAME_W; /* 168 */
  img_dev_class->img_height = 0;                       /* variable */

  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;
  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features (dev_class);
}
