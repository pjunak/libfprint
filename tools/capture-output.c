/* Private, exclusive capture output. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "capture-output.h"
#include <string.h>

gboolean
capture_write_pgm (const gchar *path, guint width, guint height,
                   const guint8 *pixels, gsize size, GError **error)
{
  if (!width || !height || width > 8192 || height > 8192 ||
      size > 16 * 1024 * 1024 || size != (gsize) width * height || !pixels)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid capture dimensions");
      return FALSE;
    }
  g_autoptr(GFile) file = g_file_new_for_path (path);
  /* Exclusive creation with mode 0600; do not replace existing files/symlinks. */
  g_autoptr(GFileOutputStream) output = g_file_create (file, G_FILE_CREATE_PRIVATE, NULL, error);
  if (!output)
    return FALSE;
  g_autofree gchar *header = g_strdup_printf ("P5\n%u %u\n255\n", width, height);
  gboolean success = g_output_stream_write_all (G_OUTPUT_STREAM (output), header, strlen (header), NULL, NULL, error) &&
                     g_output_stream_write_all (G_OUTPUT_STREAM (output), pixels, size, NULL, NULL, error) &&
                     g_output_stream_close (G_OUTPUT_STREAM (output), NULL, error);
  if (!success)
    g_file_delete (file, NULL, NULL);
  return success;
}
