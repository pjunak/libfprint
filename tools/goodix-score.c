/* Offline NBIS scoring of assembled P5 images. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "drivers_api.h"
#include "fp-print-private.h"
#include <stdio.h>
#include <errno.h>

typedef struct { GMainLoop *loop;
                 gboolean   success;
                 GError    *error;
} Detection;

static void
detected (GObject *source, GAsyncResult *result, gpointer data)
{
  Detection *d = data;

  d->success = fp_image_detect_minutiae_finish (FP_IMAGE (source), result, &d->error);
  g_main_loop_quit (d->loop);
}

/* Read one ASCII token; consume exactly its delimiter, leaving binary pixels intact. */
static gboolean
token (FILE *file, gchar value[32])
{
  int c;

  do
    {
      c = fgetc (file);
      if (c == '#')
        while (c != '\n' && c != EOF)
          c = fgetc (file);
    }
  while (c != EOF && g_ascii_isspace (c));
  guint n = 0;
  while (c != EOF && !g_ascii_isspace (c))
    {
      if (n == 31)
        return FALSE;
      value[n++] = c;
      c = fgetc (file);
    }
  if (c == '\r')
    {
      c = fgetc (file);
      if (c != '\n' && c != EOF)
        ungetc (c, file);
    }
  value[n] = 0;
  return n > 0 && c != EOF;
}

static gboolean
number (FILE *file, guint *value)
{
  gchar text[32], *end;

  if (!token (file, text) || !g_ascii_isdigit (text[0]))
    return FALSE;
  errno = 0;
  guint64 parsed = g_ascii_strtoull (text, &end, 10);
  if (errno || *end || parsed > 8192)
    return FALSE;
  *value = parsed;
  return TRUE;
}

static FpImage *
read_pgm (const gchar *path)
{
  FILE *file = fopen (path, "rb");

  if (!file)
    return NULL;
  gchar magic[32];
  guint width, height, maxval;
  FpImage *image = NULL;
  if (token (file, magic) && g_str_equal (magic, "P5") &&
      number (file, &width) && number (file, &height) && number (file, &maxval) &&
      width >= 16 && height >= 16 && width * height <= 16 * 1024 * 1024 && maxval == 255)
    {
      image = fp_image_new (width, height);
      image->ppmm = 500.0 / 25.4;
      if (fread (image->data, 1, width * height, file) != width * height || fgetc (file) != EOF)
        g_clear_object (&image);
    }
  fclose (file);
  return image;
}

int
main (int argc, char **argv)
{
  if (argc < 3 || argc > 257)
    {
      g_printerr ("Usage: %s IMAGE.pgm IMAGE.pgm [...] (2..256 assembled 500-DPI images)\n", argv[0]);
      return 2;
    }
  g_autoptr(GPtrArray) templates = g_ptr_array_new_with_free_func (g_free);
  g_print ("{\"images\":[");
  for (int i = 1; i < argc; i++)
    {
      gint64 start = g_get_monotonic_time ();
      g_autoptr(FpImage) image = read_pgm (argv[i]);
      g_autoptr(FpPrint) print = g_object_new (FP_TYPE_PRINT, "driver", "offline-nbis", "device-id", "55a2", NULL);
      g_autoptr(GError) error = NULL;
      struct xyt_struct *xyt = NULL;
      if (!image)
        {
          g_printerr ("Invalid/missing P5 8-bit image: %s\n", argv[i]);
          return 2;
        }
      g_autoptr(GMainLoop) loop = g_main_loop_new (NULL, FALSE);
      Detection d = {loop, FALSE, NULL};
      fp_image_detect_minutiae (image, NULL, detected, &d);
      g_main_loop_run (loop);
      fpi_print_set_type (print, FPI_PRINT_NBIS);
      if (d.success && fpi_print_add_from_image (print, image, &error))
        xyt = g_memdup (print->prints->pdata[0], sizeof (*xyt));
      g_clear_error (&d.error);
      g_ptr_array_add (templates, xyt);
      g_print ("%s{\"index\":%d,\"minutiae\":%d,\"extraction_ms\":%.3f,\"usable\":%s}",
               i > 1 ? "," : "", i - 1, xyt ? xyt->nrows : 0,
               (g_get_monotonic_time () - start) / 1000.0, xyt ? "true" : "false");
    }
  g_print ("],\"scores\":[");
  gboolean first = TRUE;
  for (guint i = 0; i < templates->len; i++)
    {
      struct xyt_struct *probe = g_ptr_array_index (templates, i);
      if (!probe)
        continue;
      int length = bozorth_probe_init (probe);
      for (guint j = 0; j < templates->len; j++)
        {
          struct xyt_struct *gallery = g_ptr_array_index (templates, j);
          if (i == j || !gallery)
            continue;
          int score = bozorth_to_gallery (length, probe, gallery);
          g_print ("%s{\"probe\":%u,\"gallery\":%u,\"score\":%d}", first ? "" : ",", i, j, score);
          first = FALSE;
        }
    }
  g_print ("]}\n");
  return 0;
}
