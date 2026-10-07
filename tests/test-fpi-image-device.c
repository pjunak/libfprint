/* Image-device recovery regressions. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "drivers_api.h"
#include "fp-print-private.h"

typedef struct
{
  FpImageDevice parent;
  guint         activations;
} FpiImageDeviceTest;

typedef FpImageDeviceClass FpiImageDeviceTestClass;

GType fpi_image_device_test_get_type (void);
G_DEFINE_TYPE (FpiImageDeviceTest, fpi_image_device_test, FP_TYPE_IMAGE_DEVICE)

static void
image_open (FpImageDevice *device)
{
  fpi_image_device_open_complete (device, NULL);
}

static void
image_close (FpImageDevice *device)
{
  fpi_image_device_close_complete (device, NULL);
}

static void
image_activate (FpImageDevice *device)
{
  FpiImageDeviceTest *self = (FpiImageDeviceTest *) device;

  self->activations++;
  fpi_image_device_activate_complete (device,
                                      g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                                           "Test activation failure"));
}

static void
fpi_image_device_test_init (FpiImageDeviceTest *self)
{
}

static void
fpi_image_device_test_class_init (FpiImageDeviceTestClass *class)
{
  FpDeviceClass *device_class = FP_DEVICE_CLASS (class);

  device_class->id = "image-test";
  device_class->full_name = "Image recovery test device";
  device_class->type = FP_DEVICE_TYPE_VIRTUAL;
  class->img_open = image_open;
  class->img_close = image_close;
  class->activate = image_activate;
  fpi_device_class_auto_initialize_features (device_class);
}

static void
test_activation_retry (void)
{
  g_autoptr(FpDevice) device = g_object_new (fpi_image_device_test_get_type (), NULL);
  g_autoptr(GError) error = NULL;

  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  for (guint i = 0; i < 2; i++)
    {
      FpiImageDeviceState state;
      g_assert_null (fp_device_capture_sync (device, TRUE, NULL, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_clear_error (&error);
      g_object_get (device, "fpi-image-device-state", &state, NULL);
      g_assert_cmpint (state, ==, FPI_IMAGE_DEVICE_STATE_INACTIVE);
    }
  g_assert_cmpuint (((FpiImageDeviceTest *) device)->activations, ==, 2);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
}

static void
test_enrollment_update (gconstpointer incompatible)
{
  g_autoptr(FpDevice) device = g_object_new (fpi_image_device_test_get_type (), NULL);
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = g_object_ref_sink (fp_print_new (device));
  FpiPrintType type = incompatible ? FPI_PRINT_RAW : FPI_PRINT_NBIS;
  fpi_print_set_type (print, type);
  gpointer original_data = print->prints;
  g_assert_true (fp_device_open_sync (device, NULL, &error));
  g_assert_no_error (error);
  g_assert_null (fp_device_enroll_sync (device, print, NULL, NULL, NULL, &error));
  if (incompatible)
    g_assert_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID);
  else
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED); /* Reached the test activation. */
  g_assert_cmpuint (((FpiImageDeviceTest *) device)->activations, ==, incompatible ? 0 : 1);
  g_assert_cmpint (print->type, ==, type);
  g_assert_true (print->prints == original_data);
  g_clear_error (&error);
  g_assert_true (fp_device_close_sync (device, NULL, &error));
  g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/image-device/activation-retry", test_activation_retry);
  g_test_add_data_func ("/image-device/enrollment-update", NULL, test_enrollment_update);
  g_test_add_data_func ("/image-device/enrollment-algorithm-mismatch", GINT_TO_POINTER (1), test_enrollment_update);
  return g_test_run ();
}
