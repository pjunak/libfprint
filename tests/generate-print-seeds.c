/* Synthetic print corpus seeds. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "drivers_api.h"
#include "fp-print-private.h"
int
main (int argc, char **argv)
{
  if (argc != 2 || g_mkdir_with_parents (argv[1], 0700) != 0)
    return 2;
  for (guint i = 0; i < 2; i++)
    {
      g_autoptr(FpPrint) print = g_object_new (FP_TYPE_PRINT, "driver", "synthetic", "device-id", "0", NULL);
      fpi_print_set_type (print, i ? FPI_PRINT_NBIS : FPI_PRINT_RAW);
      if (!i)
        {
          g_object_set (print, "fpi-data", g_variant_new_string ("synthetic"), NULL);
        }
      else
        {
          struct xyt_struct *xyt = g_new0 (struct xyt_struct, 1);
          xyt->nrows = 1;
          xyt->xcol[0] = 12;
          xyt->ycol[0] = 34;
          xyt->thetacol[0] = 90;
          g_ptr_array_add (print->prints, xyt);
        }
      g_autofree guint8 *bytes = NULL;
      gsize length;
      g_autoptr(GError) error = NULL;
      g_autofree gchar *path = g_build_filename (argv[1], i ? "valid-nbis" : "valid-raw", NULL);
      if (!fp_print_serialize (print, &bytes, &length, &error) ||
          !g_file_set_contents (path, (gchar *) bytes, length, &error))
        {
          g_printerr ("%s\n", error->message);
          return 1;
        }
    }
  return 0;
}
