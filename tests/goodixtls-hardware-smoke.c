/* Manual USB check, never run by meson test. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fprint.h"
#include "../tools/capture-output.h"
#include <gio/gio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean
save_capture (FpImage *image, const gchar *directory, guint cycle, guint attempt, GError **error)
{
  g_autofree gchar *name = g_strdup_printf ("capture-%04u-%u.pgm", cycle, attempt);
  g_autofree gchar *path = g_build_filename (directory, name, NULL);
  gsize length;
  const guchar *pixels = fp_image_get_data (image, &length);
  return capture_write_pgm (path, fp_image_get_width (image), fp_image_get_height (image), pixels, length, error);
}

typedef struct { GCancellable *cancellable; gboolean fired; } Cancellation;

typedef struct {
  gboolean ready;
  gboolean finger_seen;
  gboolean capture;
} Progress;

static void
finger_status_changed (FpDevice *device, GParamSpec *pspec, gpointer data)
{
  Progress *progress = data;
  FpFingerStatusFlags status = fp_device_get_finger_status (device);
  if (status & FP_FINGER_STATUS_NEEDED)
    {
      if (!progress->ready && progress->capture)
        g_printerr ("Ready: swipe your finger across the sensor, then lift it\n");
      progress->ready = TRUE;
    }
  progress->finger_seen |= (status & FP_FINGER_STATUS_PRESENT) != 0;
}
static gboolean cancel_capture (gpointer data)
{
  Cancellation *cancel = data;
  cancel->fired = TRUE;
  g_cancellable_cancel (cancel->cancellable);
  return G_SOURCE_REMOVE;
}

static gboolean watchdog (gpointer unused)
{
  g_printerr ("Hardware operation did not finish after cancellation\n");
  exit (124);
}

int main (int argc, char **argv)
{
  gint cycles = 3, delay_ms = 3000, initial_cancel_ms = 100;
  gboolean capture = FALSE;
  g_autofree gchar *scenario = NULL;
  g_autofree gchar *output_dir = NULL;
  GOptionEntry entries[] = {
    {"cycles", 0, 0, G_OPTION_ARG_INT, &cycles, "Open/close cycles; each contains two immediate attempts", "N"},
    {"delay-ms", 0, 0, G_OPTION_ARG_INT, &delay_ms, "Cancellation deadline", "MS"},
    {"initial-cancel-ms", 0, 0, G_OPTION_ARG_INT, &initial_cancel_ms, "First cancellation deadline (use 5000 for a setup trace)", "MS"},
    {"capture", 0, 0, G_OPTION_ARG_NONE, &capture, "Expect a capture instead of cancellation; images are discarded", NULL},
    {"output-dir", 0, 0, G_OPTION_ARG_FILENAME, &output_dir, "Save captures in an existing private directory (requires --capture)", "DIR"},
    {"scenario", 0, 0, G_OPTION_ARG_STRING, &scenario, "steady, cold-boot, warm-boot or resume", "NAME"},
    {NULL},
  };
  g_autoptr(GOptionContext) options = g_option_context_new ("- manual Goodix lifecycle check");
  g_autoptr(GError) error = NULL;
  g_option_context_add_main_entries (options, entries, NULL);
  if (!g_option_context_parse (options, &argc, &argv, &error)) {
    g_printerr ("%s\n", error->message);
    return 2;
  }
  const gchar *label = scenario ? scenario : "steady";
  if (cycles < 1 || cycles > 1000 || delay_ms < 1 || delay_ms > 60000 ||
      initial_cancel_ms < 1 || initial_cancel_ms > 60000 || argc != 1 ||
      !(g_str_equal (label, "steady") || g_str_equal (label, "cold-boot") ||
        g_str_equal (label, "warm-boot") || g_str_equal (label, "resume"))) return 2;
  if (output_dir)
    {
      struct stat info;
      if (!capture || lstat (output_dir, &info) || !S_ISDIR (info.st_mode) ||
          info.st_uid != geteuid () || (info.st_mode & 077))
        {
          g_printerr ("--output-dir requires --capture and an owned directory with mode 0700\n");
          return 2;
        }
    }
  g_autoptr(FpContext) context = fp_context_new ();
  fp_context_enumerate (context);
  GPtrArray *devices = fp_context_get_devices (context);
  FpDevice *dev = NULL;
  for (guint i = 0; i < devices->len; i++) {
    FpDevice *candidate = g_ptr_array_index (devices, i);
    if (g_strcmp0 (fp_device_get_driver (candidate), "goodixtls55x4") == 0)
      {
        if (dev) { g_printerr ("Expected exactly one Goodix reader\n"); return 1; }
        dev = candidate;
      }
  }
  if (!dev) { g_printerr ("No Goodix reader found\n"); return 1; }
  for (gint round = 0; round < cycles; round++) {
    guint guard = g_timeout_add (delay_ms + 15000, watchdog, NULL);
    if (!fp_device_open_sync (dev, NULL, &error)) {
      g_source_remove (guard);
      g_printerr ("Open failed: %s\n", error->message);
      return 1;
    }
    g_source_remove (guard);
    gboolean passed = TRUE;
    for (guint attempt = 0; attempt < 2; attempt++) {
      g_autoptr(GCancellable) cancellable = g_cancellable_new ();
      Cancellation cancel = {cancellable, FALSE};
      guint deadline = !capture && round == 0 && attempt == 0 ? initial_cancel_ms : delay_ms;
      guint timer = g_timeout_add (deadline, cancel_capture, &cancel);
      guard = g_timeout_add (deadline + 15000, watchdog, NULL);
      gint64 start = g_get_monotonic_time ();
      Progress progress = {.capture = capture};
      gulong handler = g_signal_connect (dev, "notify::finger-status",
                                         G_CALLBACK (finger_status_changed), &progress);
      if (capture)
        g_printerr ("Cycle %d, sample %u of 2: keep the sensor clear until Ready\n", round + 1, attempt + 1);
      g_autoptr(FpImage) image = fp_device_capture_sync (dev, TRUE, cancellable, &error);
      g_signal_handler_disconnect (dev, handler);
      if (!cancel.fired) g_source_remove (timer);
      g_source_remove (guard);
      passed = capture ? image != NULL : g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      /* Only the first attempt deliberately cancels before setup. The soak
       * must not count repeated initialization hangs as successful tests. */
      if (!capture && !(round == 0 && attempt == 0) && !progress.ready)
        {
          passed = FALSE;
          g_printerr ("Reader did not finish initialization/calibration before the deadline\n");
        }
      if (passed && output_dir) passed = save_capture (image, output_dir, round + 1, attempt + 1, &error);
      g_print ("{\"scenario\":\"%s\",\"cycle\":%d,\"attempt\":%u,\"mode\":\"%s\",\"passed\":%s,\"ready\":%s,\"finger_seen\":%s,\"elapsed_ms\":%.3f}\n",
               label, round + 1, attempt + 1, capture ? "capture" : "cancel", passed ? "true" : "false",
               progress.ready ? "true" : "false", progress.finger_seen ? "true" : "false",
               (g_get_monotonic_time () - start) / 1000.0);
      if (!passed && error) g_printerr ("Capture failed: %s\n", error->message);
      g_clear_error (&error);
      if (!passed) break;
    }
    guard = g_timeout_add (15000, watchdog, NULL);
    gboolean closed = fp_device_close_sync (dev, NULL, &error);
    g_source_remove (guard);
    if (!closed) g_printerr ("Close failed: %s\n", error->message);
    if (!passed || !closed) return 1;
  }
  return 0;
}
