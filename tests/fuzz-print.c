/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdint.h>
#include "fprint.h"
int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (size > 1024 * 1024) return 0;
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) print = fp_print_deserialize (data, size, &error);
  return 0;
}
