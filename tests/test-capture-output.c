/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../tools/capture-output.h"
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void
test_capture_output (void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *directory = g_dir_make_tmp ("goodix-capture-test-XXXXXX", &error);
  g_assert_no_error (error);
  g_autofree gchar *path = g_build_filename (directory, "image.pgm", NULL);
  g_autofree gchar *link = g_build_filename (directory, "link.pgm", NULL);
  guint8 pixels[] = {10, 13, 0, 255};
  g_assert_true (capture_write_pgm (path, 2, 2, pixels, sizeof (pixels), &error));
  g_assert_no_error (error);
  struct stat info;
  g_assert_cmpint (g_stat (path, &info), ==, 0);
  g_assert_cmpint (info.st_mode & 0777, ==, 0600);

  g_autofree gchar *data = NULL;
  gsize size;
  static const guint8 expected[] = "P5\n2 2\n255\n\x0a\x0d\x00\xff";
  g_assert_true (g_file_get_contents (path, &data, &size, &error));
  g_assert_no_error (error);
  g_assert_cmpmem (data, size, expected, sizeof (expected) - 1);
  g_assert_false (capture_write_pgm (path, 2, 2, pixels, sizeof (pixels), &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);
  g_assert_cmpint (symlink (path, link), ==, 0);
  g_assert_false (capture_write_pgm (link, 2, 2, pixels, sizeof (pixels), &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);
  g_assert_cmpint (g_unlink (link), ==, 0);
  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_false (capture_write_pgm (path, 2, 3, pixels, sizeof (pixels), &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/capture/private-exclusive-output", test_capture_output);
  return g_test_run ();
}
