/* Print storage/ownership regressions. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "drivers_api.h"
#include "fp-print-private.h"
#include "test-device-fake.h"

static FpPrint *
new_print (FpiPrintType type)
{
  g_autoptr(FpDevice) device = g_object_new (FPI_TYPE_DEVICE_FAKE, NULL);
  FpPrint *print = fp_print_new (device);
  fpi_print_set_type (print, type);
  return print;
}

static FpImage *
new_sigfm_image (void)
{
  FpImage *image = fp_image_new (64, 64);
  /* Blank synthetic data: tests allocation ownership, not matching accuracy. */
  image->sigfm_info = sigfm_extract (image->data, image->width, image->height);
  g_assert_nonnull (image->sigfm_info);
  return image;
}

static void
test_print_roundtrip (gconstpointer user_data)
{
  FpiPrintType type = GPOINTER_TO_INT (user_data);
  g_autoptr(FpPrint) print = new_print (type);
  g_autoptr(FpPrint) restored = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree guint8 *bytes = NULL;
  gsize length;

  if (type == FPI_PRINT_RAW)
    g_object_set (print, "fpi-data", g_variant_new_string ("synthetic print"), NULL);
  else if (type == FPI_PRINT_NBIS)
    {
      struct xyt_struct *xyt = g_new0 (struct xyt_struct, 1);
      xyt->nrows = 1;
      xyt->xcol[0] = 12;
      xyt->ycol[0] = 34;
      xyt->thetacol[0] = 90;
      g_ptr_array_add (print->prints, xyt);
    }
  else
    {
      g_autoptr(FpImage) image = new_sigfm_image ();
      g_assert_true (fpi_print_add_from_image (print, image, &error));
      g_assert_no_error (error);
    }

  g_assert_true (fp_print_serialize (print, &bytes, &length, &error));
  g_assert_no_error (error);
  restored = fp_print_deserialize (bytes, length, &error);
  g_assert_no_error (error);
  g_assert_nonnull (restored);
  g_assert_true (fp_print_equal (print, restored));
}

#if HAVE_SIGFM
static void
test_sigfm_ownership (void)
{
  g_autoptr(FpImage) image = new_sigfm_image ();
  g_autoptr(FpPrint) first = new_print (FPI_PRINT_SIGFM);
  g_autoptr(FpPrint) second = new_print (FPI_PRINT_SIGFM);
  g_autoptr(GError) error = NULL;

  g_assert_true (fpi_print_add_from_image (first, image, &error));
  g_assert_no_error (error);
  g_assert_true (fpi_print_add_from_image (second, image, &error));
  g_assert_no_error (error);
  g_assert_true (image->sigfm_info != first->prints->pdata[0]);
  g_assert_true (image->sigfm_info != second->prints->pdata[0]);
  g_assert_true (first->prints->pdata[0] != second->prints->pdata[0]);

  g_clear_object (&first);
  g_assert_cmpint (sigfm_keypoints_count (image->sigfm_info), ==, 0);
  g_assert_cmpint (sigfm_keypoints_count (second->prints->pdata[0]), ==, 0);
}

#endif

static void
test_missing_sigfm_features (void)
{
  g_autoptr(FpImage) image = fp_image_new (64, 64);
  g_autoptr(FpPrint) print = new_print (FPI_PRINT_SIGFM);
  g_autoptr(GError) error = NULL;
  g_assert_false (fpi_print_add_from_image (print, image, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (print->prints->len, ==, 0);
}

static void
test_short_print (void)
{
  const guint8 bytes[] = "FP3";
  g_autoptr(GError) error = NULL;
  for (guint length = 0; length <= 3; length++)
    {
      g_assert_null (fp_print_deserialize (bytes, length, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
      g_clear_error (&error);
    }
}

static void
test_wrong_print_schema (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) value = g_variant_ref_sink (g_variant_new (
      "(issbymsmsi@a{sv}v)", GPOINTER_TO_INT (user_data), "test", "0", FALSE,
      FP_FINGER_UNKNOWN, NULL, NULL, 1,
      g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
      g_variant_new_string ("wrong nested schema")));
  gsize length = 3 + g_variant_get_size (value);
  g_autofree guint8 *bytes = g_malloc (length);
  memcpy (bytes, "FP3", 3);
  memcpy (bytes + 3, g_variant_get_data (value), length - 3);
  g_assert_null (fp_print_deserialize (bytes, length, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_invalid_metadata (void)
{
  const struct { guint8 finger; gint date; } cases[] = {{255, 1}, {0, 0}, {0, -1}, {0, G_MAXINT32}};
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    g_autoptr(GVariant) value = g_variant_ref_sink (g_variant_new (
      "(issbymsmsi@a{sv}v)", FPI_PRINT_RAW, "test", "0", FALSE,
      cases[i].finger, NULL, NULL, cases[i].date,
      g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0),
      g_variant_new_variant (g_variant_new_string ("synthetic"))));
    gsize length = 3 + g_variant_get_size (value);
    g_autofree guint8 *bytes = g_malloc (length);
    memcpy (bytes, "FP3", 3);
    memcpy (bytes + 3, g_variant_get_data (value), length - 3);
    g_autoptr(GError) error = NULL;
    g_assert_null (fp_print_deserialize (bytes, length, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  }
}

static void
test_empty_minutiae (void)
{
  g_autoptr(FpPrint) print = new_print (FPI_PRINT_NBIS);
  g_ptr_array_add (print->prints, g_new0 (struct xyt_struct, 1));
  g_autofree guint8 *data = NULL;
  gsize length;
  g_autoptr(GError) error = NULL;
  g_assert_true (fp_print_serialize (print, &data, &length, &error));
  g_assert_null (fp_print_deserialize (data, length, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

#if !HAVE_SIGFM
static void
test_disabled_sigfm (void)
{
  g_autoptr(FpPrint) print = new_print (FPI_PRINT_SIGFM);
  g_autoptr(GError) error = NULL;
  g_autofree guint8 *data = NULL;
  gsize length;
  g_assert_false (fp_print_serialize (print, &data, &length, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&error);
  g_autoptr(GVariant) nested = g_variant_ref_sink (g_variant_new ("(@a(ay))",
    g_variant_new_array (G_VARIANT_TYPE ("(ay)"), NULL, 0)));
  g_autoptr(GVariant) value = g_variant_ref_sink (g_variant_new (
    "(issbymsmsi@a{sv}v)", FPI_PRINT_SIGFM, "test", "0", FALSE,
    FP_FINGER_UNKNOWN, NULL, NULL, 1,
    g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0), nested));
  length = 3 + g_variant_get_size (value);
  data = g_malloc (length);
  memcpy (data, "FP3", 3);
  memcpy (data + 3, g_variant_get_data (value), length - 3);
  g_assert_null (fp_print_deserialize (data, length, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}
#endif

static FpPrint *
load_fixture (const gchar *name, gchar **contents, gsize *length)
{
  g_autofree gchar *path = g_build_filename (g_getenv ("MESON_SOURCE_ROOT"), "tests", "prints", name, NULL);
  g_autoptr(GError) error = NULL;
  FpPrint *print;

  g_assert_true (g_file_get_contents (path, contents, length, &error));
  print = fp_print_deserialize ((guchar *) *contents, *length, &error);
  g_assert_no_error (error);
  g_assert_nonnull (print);
  return print;
}

/* Prints enrolled with the published 072991a build must keep loading after
 * an upgrade. The fixtures are byte-for-byte what that build's
 * fp_print_serialize() writes (libfprint 1.94's "FP3" format); their minutiae
 * are synthetic. One carries an enrollment date, one the "no date" marker. */
static void
test_published_print_format (void)
{
  g_autofree gchar *dated_bytes = NULL;
  g_autofree gchar *undated_bytes = NULL;
  g_autofree guchar *saved = NULL;
  g_autoptr(GError) error = NULL;
  gsize dated_length, undated_length, saved_length;
  g_autoptr(FpPrint) dated = load_fixture ("goodix-072991a-nbis.print", &dated_bytes, &dated_length);
  g_autoptr(FpPrint) undated = load_fixture ("goodix-072991a-nbis-nodate.print", &undated_bytes, &undated_length);
  const GDate *date;
  struct xyt_struct *xyt;

  g_assert_cmpstr (fp_print_get_driver (dated), ==, "goodixtls55x4");
  g_assert_cmpstr (fp_print_get_device_id (dated), ==, "3-3");
  g_assert_cmpint (fp_print_get_finger (dated), ==, FP_FINGER_RIGHT_INDEX);
  g_assert_cmpstr (fp_print_get_username (dated), ==, "alice");
  g_assert_null (fp_print_get_description (dated));
  g_assert_cmpint (dated->type, ==, FPI_PRINT_NBIS);
  g_assert_cmpuint (dated->prints->len, ==, 1);
  xyt = g_ptr_array_index (dated->prints, 0);
  g_assert_cmpint (xyt->nrows, ==, 4);
  g_assert_cmpint (xyt->xcol[3], ==, 120);
  g_assert_cmpint (xyt->ycol[1], ==, 60);
  g_assert_cmpint (xyt->thetacol[2], ==, 180);
  date = fp_print_get_enroll_date (dated);
  g_assert_nonnull (date);
  g_assert_cmpint (g_date_get_year (date), ==, 2026);
  g_assert_cmpint (g_date_get_month (date), ==, G_DATE_OCTOBER);
  g_assert_cmpint (g_date_get_day (date), ==, 7);
  g_assert_null (fp_print_get_enroll_date (undated));

  /* Saving again produces the same bytes: the format has not drifted. */
  g_assert_true (fp_print_serialize (dated, &saved, &saved_length, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (saved, saved_length, dated_bytes, dated_length);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/print/roundtrip/raw", GINT_TO_POINTER (FPI_PRINT_RAW), test_print_roundtrip);
  g_test_add_data_func ("/print/roundtrip/nbis", GINT_TO_POINTER (FPI_PRINT_NBIS), test_print_roundtrip);
#if HAVE_SIGFM
  g_test_add_data_func ("/print/roundtrip/sigfm", GINT_TO_POINTER (FPI_PRINT_SIGFM), test_print_roundtrip);
  g_test_add_func ("/print/sigfm-ownership", test_sigfm_ownership);
#endif
  g_test_add_func ("/print/missing-sigfm-features", test_missing_sigfm_features);
  g_test_add_func ("/print/short-data", test_short_print);
  g_test_add_data_func ("/print/wrong-schema/raw", GINT_TO_POINTER (FPI_PRINT_RAW), test_wrong_print_schema);
  g_test_add_data_func ("/print/wrong-schema/nbis", GINT_TO_POINTER (FPI_PRINT_NBIS), test_wrong_print_schema);
  g_test_add_data_func ("/print/wrong-schema/sigfm", GINT_TO_POINTER (FPI_PRINT_SIGFM), test_wrong_print_schema);
#if !HAVE_SIGFM
  g_test_add_func ("/print/disabled-sigfm", test_disabled_sigfm);
#endif
  g_test_add_func ("/print/invalid-metadata", test_invalid_metadata);
  g_test_add_func ("/print/empty-minutiae", test_empty_minutiae);
  g_test_add_func ("/print/published-format", test_published_print_format);
  return g_test_run ();
}
