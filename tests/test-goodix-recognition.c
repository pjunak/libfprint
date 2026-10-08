/* Recognition regression for the 55a2 swipe pipeline.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Real fingerprints (other drivers' public test captures) are swiped across a
 * simulated 55a2: each frame is the 12-bit background minus the finger's
 * signal, with a strong position-dependent response like a real reader's.
 * The images go through the driver's own swipe code and NBIS, and the
 * enrolled finger must match while the others must not.
 *
 * On real captures, the old pipeline (frames stacked edge to edge, no
 * normalisation) let other fingers match. Here it, and stitching without
 * normalisation, fail because the enrolled finger no longer matches. This is
 * a guard against that class of bug, not a measure of accuracy. */
#include "drivers_api.h"
#include "fpi-print.h"
#include "drivers/goodixtls/goodix-swipe.h"
#include <cairo.h>

#define THRESHOLD 40 /* goodixtls55x4's bz3_threshold */
#define ENROLL_SWIPES 6 /* and its nr_enroll_stages */
#define BACKGROUND 3500

typedef struct
{
  const gchar *name;
  guint        width, height;
  guint8      *ridges; /* 0 = valley, 255 = ridge */
} Finger;

/* The first ENROLLED fixtures are enrolled and must match themselves; every
 * finger is also probed against them and must not match the others. All
 * have enough texture across the sensor for complete simulated swipes. */
#define ENROLLED 2
static Finger fingers[] = {
  {"aes3500"}, {"egis0570"}, {"elanspi"}, {"uru4000-4500"}, {"vfs301"}, {"vfs7552"},
};

static void
load_finger (Finger *finger)
{
  g_autofree gchar *path = g_build_filename (g_getenv ("MESON_SOURCE_ROOT"), "tests",
                                             finger->name, "capture.png", NULL);
  cairo_surface_t *surface = cairo_image_surface_create_from_png (path);

  g_assert_cmpint (cairo_surface_status (surface), ==, CAIRO_STATUS_SUCCESS);
  finger->width = cairo_image_surface_get_width (surface);
  finger->height = cairo_image_surface_get_height (surface);
  finger->ridges = g_malloc (finger->width * finger->height);
  const guint8 *data = cairo_image_surface_get_data (surface);
  gint stride = cairo_image_surface_get_stride (surface);
  guint histogram[256] = { 0 };
  guint low = 0, high = 255, count = 0;
  for (guint y = 0; y < finger->height; y++)
    for (guint x = 0; x < finger->width; x++)
      {
        guint32 pixel;
        memcpy (&pixel, data + y * stride + x * 4, sizeof (pixel));
        finger->ridges[y * finger->width + x] = 255 - (pixel & 0xff);
        histogram[255 - (pixel & 0xff)]++;
      }
  cairo_surface_destroy (surface);

  /* The fixtures come from different sensors: give them the same contrast. */
  for (guint v = 0, n = finger->width * finger->height; v < 256; v++)
    {
      count += histogram[v];
      if (count <= n / 20)
        low = v;
      if (count < n * 19 / 20)
        high = v + 1;
    }
  for (guint i = 0; i < finger->width * finger->height; i++)
    finger->ridges[i] = CLAMP (((gint) finger->ridges[i] - (gint) low) * 255 /
                               (gint) MAX (high - low, 1), 0, 255);
}

/* The sensor's response: a large offset and ridge gain that both change
 * smoothly with position, like the mean frame of a real 55a2 (one end of the
 * long axis near saturation, the middle of the short axis much weaker). */
static void
response (guint sx, guint sy, gint *offset, gint *gain)
{
  double along = sy / (double) (GOODIX55X4_HEIGHT - 1);
  double across = (sx - (GOODIX55X4_WIDTH - 1) / 2.0) / (GOODIX55X4_WIDTH / 2.0);

  *offset = 450 + 900 * along * along + 500 * across * across;
  *gain = 250 + 650 * along + 300 * across * across;
}

/* One sensor frame with the finger's top row at @position and its left
 * column at @lateral; NULL finger = empty sensor. */
static void
sensor_frame (const Finger *finger, gint position, gint lateral, Goodix55X4Pix *frame)
{
  for (guint sy = 0; sy < GOODIX55X4_HEIGHT; sy++)
    for (guint sx = 0; sx < GOODIX55X4_WIDTH; sx++)
      {
        gint value = BACKGROUND;
        if (finger)
          {
            gint offset, gain;
            gint fx = CLAMP ((gint) sy + lateral, 0, (gint) finger->width - 1);
            gint fy = CLAMP ((gint) sx + position, 0, (gint) finger->height - 1);
            response (sx, sy, &offset, &gain);
            value -= offset + gain * finger->ridges[fy * finger->width + fx] / 255;
          }
        frame[sy * GOODIX55X4_WIDTH + sx] = value;
      }
}

/* One swipe from top to bottom; NULL if the driver would ask for another. */
static FpImage *
try_swipe (const Finger *finger, GRand *rand)
{
  g_autofree Goodix55X4Pix *background = g_new (Goodix55X4Pix, GOODIX55X4_FRAME_SIZE);
  g_autofree Goodix55X4Pix *frame = g_new (Goodix55X4Pix, GOODIX55X4_FRAME_SIZE);
  GoodixSwipe state = { 0 };
  GoodixSwipeResult result = GOODIX_SWIPE_CONTINUE;
  gint position = g_rand_int_range (rand, 0, 12);
  gint lateral = (finger->width - GOODIX55X4_HEIGHT) / 2 + g_rand_int_range (rand, -8, 9);
  gint step = g_rand_int_range (rand, 5, 10);
  FpImage *image = NULL;

  sensor_frame (NULL, 0, 0, background);
  goodix_swipe_start (&state, BACKGROUND);
  for (; position + GOODIX55X4_WIDTH <= (gint) finger->height; position += step)
    {
      sensor_frame (finger, position, lateral, frame);
      result = goodix_swipe_feed (&state, frame, background);
      if (result == GOODIX_SWIPE_COMPLETE || result == GOODIX_SWIPE_TOO_SHORT)
        break;
      lateral += g_rand_int_range (rand, -1, 2);
      step = CLAMP (step + g_rand_int_range (rand, -2, 3), 3, 12);
    }
  if (result != GOODIX_SWIPE_COMPLETE && result != GOODIX_SWIPE_TOO_SHORT)
    {
      sensor_frame (NULL, 0, 0, frame); /* lifted */
      result = goodix_swipe_feed (&state, frame, background);
    }
  if (result == GOODIX_SWIPE_COMPLETE)
    image = goodix_swipe_take_image (&state);
  goodix_swipe_clear (&state);
  return image;
}

static FpImage *
swipe (const Finger *finger, GRand *rand)
{
  for (guint attempt = 0; attempt < 5; attempt++)
    {
      FpImage *image = try_swipe (finger, rand);
      if (image)
        return image;
    }
  g_error ("No complete swipe of %s", finger->name);
}

static void
minutiae_done (GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GError) error = NULL;

  g_assert_true (fp_image_detect_minutiae_finish (FP_IMAGE (source), result, &error));
  g_assert_no_error (error);
  *(gboolean *) data = TRUE;
}

static FpPrint *
to_print (FpImage *image)
{
  g_autoptr(GError) error = NULL;
  gboolean done = FALSE;
  FpPrint *print = g_object_new (FP_TYPE_PRINT, "driver", "goodixtls55x4", "device-id", "test", NULL);

  fp_image_detect_minutiae (image, NULL, minutiae_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
  fpi_print_set_type (print, FPI_PRINT_NBIS);
  g_assert_true (fpi_print_add_from_image (print, image, &error));
  g_assert_no_error (error);
  return print;
}

static FpiMatchResult
match (FpPrint *enrolled, FpPrint *probe)
{
  g_autoptr(GError) error = NULL;
  FpiMatchResult result = fpi_print_bz3_match (enrolled, probe, THRESHOLD, &error);

  g_assert_no_error (error);
  return result;
}

static void
test_recognition (void)
{
  g_autoptr(GRand) rand = g_rand_new_with_seed (55);
  FpPrint *enrolled[ENROLLED];
  FpPrint *probes[G_N_ELEMENTS (fingers)];

  for (guint f = 0; f < G_N_ELEMENTS (fingers); f++)
    {
      load_finger (&fingers[f]);
      if (f < ENROLLED)
        {
          /* One template from six swipes, like the driver's enrollment. */
          enrolled[f] = g_object_new (FP_TYPE_PRINT, "driver", "goodixtls55x4", "device-id", "test", NULL);
          fpi_print_set_type (enrolled[f], FPI_PRINT_NBIS);
          for (guint i = 0; i < ENROLL_SWIPES; i++)
            {
              g_autoptr(FpImage) image = swipe (&fingers[f], rand);
              g_autoptr(FpPrint) sample = to_print (image);
              fpi_print_add_print (enrolled[f], sample);
            }
        }
      g_autoptr(FpImage) image = swipe (&fingers[f], rand);
      probes[f] = to_print (image);
    }

  for (guint e = 0; e < ENROLLED; e++)
    for (guint p = 0; p < G_N_ELEMENTS (fingers); p++)
      {
        FpiMatchResult expected = e == p ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL;
        g_test_message ("enrolled %s, probe %s", fingers[e].name, fingers[p].name);
        g_assert_cmpint (match (enrolled[e], probes[p]), ==, expected);
      }

  for (guint f = 0; f < G_N_ELEMENTS (fingers); f++)
    {
      if (f < ENROLLED)
        g_object_unref (enrolled[f]);
      g_object_unref (probes[f]);
      g_free (fingers[f].ridges);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix/recognition/other-fingers-rejected", test_recognition);
  return g_test_run ();
}
