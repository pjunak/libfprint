/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdint.h>
#include "drivers/goodixtls/goodix_proto.h"
int LLVMFuzzerTestOneInput (const uint8_t *data,
                            size_t         size);
int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  GoodixPacket packet;

  if (size > 65539)
    return 0;
  goodix_packet_peek (data, size, &packet);
  g_autofree guint8 *payload = NULL;
  guint16 length;
  guint8 cmd;
  gboolean valid, null_checksum;
  goodix_decode_protocol ((guint8 *) data, size, &cmd, &payload, &length, &valid, &null_checksum);
  return 0;
}
