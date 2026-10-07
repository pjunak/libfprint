/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "drivers/goodixtls/goodix_proto.h"
#include "drivers/goodixtls/goodix-profiles.h"
#include "drivers/goodixtls/goodix-image.h"

static void test_profiles (void)
{
  const GoodixProfile *p = goodix_profile_lookup (0x55a2, "GF3206_RTSEC_APP_10062");
  const GoodixProfile *older = goodix_profile_lookup (0x55a2, "GF3206_RTSEC_APP_10052");
  g_assert_nonnull (p);
  g_assert_nonnull (older);
  g_assert_cmpstr (older->firmware, ==, "GF3206_RTSEC_APP_10052");
  g_assert_cmpuint (older->width, ==, p->width);
  g_assert_cmpuint (older->height, ==, p->height);
  g_assert_cmpuint (older->crop, ==, p->crop);
  g_assert_cmpuint (older->reset_number, ==, p->reset_number);
  g_assert_cmpuint (older->tls_settle_ms, ==, p->tls_settle_ms);
  g_assert_cmpmem (older->config, older->config_length, p->config, p->config_length);
  g_assert_cmpuint (p->width * p->height * 3 / 2, ==, GOODIX55X4_RAW_FRAME_SIZE);
  g_assert_null (goodix_profile_lookup (0x55a4, p->firmware));
  g_assert_null (goodix_profile_lookup (0x55a4, older->firmware));
  g_assert_null (goodix_profile_lookup (0x55b4, "GF3268_RTSEC_APP_10041"));
  g_assert_null (goodix_profile_lookup (0x55a2, "GF3206_RTSEC_APP_10053"));
  g_assert_null (goodix_profile_lookup (0x55a2, "GF3206_RTSEC_APP_10052_extra"));
  g_assert_null (goodix_profile_lookup (0x55a2, "GF3206_RTSEC_APP_99999"));
  g_assert_null (goodix_profile_lookup (0x55a2, NULL));
}

static void test_wire (void)
{
  g_autofree guint8 *payload = g_malloc0 (G_MAXUINT16);
  g_autofree guint8 *bytes = NULL;
  guint32 length;
  goodix_encode_pack (GOODIX_FLAGS_TLS, payload, G_MAXUINT16, TRUE, &bytes, &length);
  for (guint i = 0; i < 4 + G_MAXUINT16; i++) {
    GoodixPacket p;
    g_assert_cmpint (goodix_packet_peek (bytes, i, &p), ==, 0);
  }
  GoodixPacket p;
  g_assert_cmpint (goodix_packet_peek (bytes, length, &p), ==, 1);
  g_assert_cmpuint (p.length, ==, G_MAXUINT16);
  g_assert_cmpuint (p.consumed, ==, 4 + G_MAXUINT16);
  bytes[3] ^= 1;
  g_assert_cmpint (goodix_packet_peek (bytes, length, &p), ==, -1);
  const guint8 padding[] = {0, 0, 0};
  g_assert_cmpint (goodix_packet_peek (padding, sizeof (padding), &p), ==, 0);
  g_assert_cmpuint (p.consumed, ==, sizeof (padding));
}

static void test_image (void)
{
  guint8 raw[GOODIX55X4_RAW_FRAME_SIZE];
  Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE];
  guint8 out[GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT];
  memset (raw, 0xff, sizeof (raw));
  goodix_image_decode_frame (frame, raw);
  g_assert_cmpint (goodix_image_swipe_raw_mean (frame), ==, 4095);
  goodix_image_swipe_build_out (frame, out);
  for (gsize i = 0; i < sizeof (out); i++) g_assert_cmpuint (out[i], ==, 255);
  goodix_image_swipe_fpn_stretch (out);
  g_assert_cmpuint (goodix_image_swipe_out_diff (out, out), ==, 0);
  goodix_image_postprocess_frame (frame, frame);
  g_assert_cmpint (goodix_image_swipe_raw_mean (frame), ==, 0);
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/goodix/profiles", test_profiles);
  g_test_add_func ("/goodix/wire-boundaries", test_wire);
  g_test_add_func ("/goodix/image-pipeline", test_image);
  return g_test_run ();
}
