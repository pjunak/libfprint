/* Goodix TLS transport regressions. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "drivers/goodixtls/goodixtls.h"
#include <openssl/err.h>
#include <string.h>

typedef struct
{
  GoodixTlsServer server;
  SSL_CTX *ctx;
  SSL *client;
} TlsPair;

static unsigned int
client_psk (SSL *ssl, const char *hint, char *identity,
             unsigned int identity_len, unsigned char *psk, unsigned int psk_len)
{
  g_strlcpy (identity, "test sensor", identity_len);
  g_assert_cmpuint (psk_len, >=, 32);
  memset (psk, GPOINTER_TO_INT (SSL_get_app_data (ssl)), 32);
  return 32;
}

static void
pair_init (TlsPair *pair, gboolean wrong_key)
{
  g_autoptr(GError) error = NULL;
  BIO *input = BIO_new (BIO_s_mem ());
  BIO *output = BIO_new (BIO_s_mem ());

  g_assert_true (goodix_tls_server_init (&pair->server, &error));
  g_assert_no_error (error);
  pair->ctx = SSL_CTX_new (TLS_client_method ());
  g_assert_nonnull (pair->ctx);
  SSL_CTX_set_security_level (pair->ctx, 0);
  g_assert_cmpint (SSL_CTX_set_cipher_list (pair->ctx, "PSK-AES128-CBC-SHA256"), ==, 1);
  SSL_CTX_set_min_proto_version (pair->ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (pair->ctx, TLS1_2_VERSION);
  SSL_CTX_set_psk_client_callback (pair->ctx, client_psk);
  pair->client = SSL_new (pair->ctx);
  g_assert_nonnull (pair->client);
  SSL_set_app_data (pair->client, GINT_TO_POINTER (wrong_key));
  BIO_set_mem_eof_return (input, -1);
  BIO_set_mem_eof_return (output, -1);
  SSL_set_bio (pair->client, input, output);
  SSL_set_connect_state (pair->client);
}

static void
pair_clear (TlsPair *pair)
{
  goodix_tls_server_deinit (&pair->server, NULL);
  /* Cleanup must also be safe after partial initialization/previous cleanup. */
  goodix_tls_server_deinit (&pair->server, NULL);
  SSL_free (pair->client);
  SSL_CTX_free (pair->ctx);
}

static gboolean
pair_handshake (TlsPair *pair, guint fragment, GError **error)
{
  gboolean server_done = FALSE;
  guint8 buffer[8192];

  for (guint i = 0; i < 100; i++)
    {
      int result;
      int count;
      ERR_clear_error ();
      result = SSL_do_handshake (pair->client);
      if (result != 1)
        g_assert_cmpint (SSL_get_error (pair->client, result), ==, SSL_ERROR_WANT_READ);

      while (BIO_ctrl_pending (SSL_get_wbio (pair->client)))
        {
          count = BIO_read (SSL_get_wbio (pair->client), buffer, MIN (fragment, sizeof (buffer)));
          g_assert_cmpint (count, >, 0);
          g_assert_cmpint (goodix_tls_client_send (&pair->server, buffer, count), ==, count);
          if (!goodix_tls_server_handshake (&pair->server, &server_done, error))
            return FALSE;
          while ((count = goodix_tls_client_recv (&pair->server, buffer, sizeof (buffer))) > 0)
            g_assert_cmpint (BIO_write (SSL_get_rbio (pair->client), buffer, count), ==, count);
        }
      if (server_done && SSL_is_init_finished (pair->client))
        return TRUE;
    }
  g_assert_not_reached ();
}

static void
test_handshake (gconstpointer data)
{
  TlsPair pair = {0};
  g_autoptr(GError) error = NULL;
  pair_init (&pair, FALSE);
  g_assert_true (pair_handshake (&pair, GPOINTER_TO_UINT (data), &error));
  g_assert_no_error (error);
  pair_clear (&pair);
}

static void
test_wrong_key (void)
{
  TlsPair pair = {0};
  g_autoptr(GError) error = NULL;
  pair_init (&pair, TRUE);
  g_assert_false (pair_handshake (&pair, 8192, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (error->message);
  pair_clear (&pair);
}

static void
test_aborted_handshake (void)
{
  GoodixTlsServer server = {0};
  g_autoptr(GError) error = NULL;
  gboolean complete;

  for (guint i = 0; i < 100; i++)
    {
      g_assert_true (goodix_tls_server_init (&server, &error));
      g_assert_true (goodix_tls_server_handshake (&server, &complete, &error));
      g_assert_false (complete);
      g_assert_no_error (error);
      goodix_tls_server_deinit (&server, NULL);
    }
}

static void
test_image (gconstpointer data)
{
  TlsPair pair = {0};
  g_autoptr(GError) error = NULL;
  guint8 image[14784], decoded[30000], encrypted[32000];
  gboolean corrupt = GPOINTER_TO_INT (data);
  int count;

  pair_init (&pair, FALSE);
  g_assert_true (pair_handshake (&pair, 8192, &error));
  for (guint i = 0; i < sizeof (image); i++)
    image[i] = i % 251;
  /* Two application records in a single USB packet. */
  g_assert_cmpint (SSL_write (pair.client, image, 5000), ==, 5000);
  g_assert_cmpint (SSL_write (pair.client, image + 5000, sizeof (image) - 5000), ==, sizeof (image) - 5000);
  count = BIO_read (SSL_get_wbio (pair.client), encrypted, sizeof (encrypted));
  g_assert_cmpint (count, >, 0);
  if (corrupt)
    encrypted[count - 1] ^= 0x42;
  g_assert_cmpint (goodix_tls_client_send (&pair.server, encrypted, count), ==, count);
  count = goodix_tls_server_receive (&pair.server, decoded, sizeof (decoded), &error);
  if (corrupt)
    {
      g_assert_cmpint (count, ==, -1);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    }
  else
    {
      g_assert_cmpint (count, ==, sizeof (image));
      g_assert_no_error (error);
      g_assert_cmpmem (decoded, count, image, sizeof (image));
      count = goodix_tls_server_receive (&pair.server, decoded, sizeof (decoded), &error);
      g_assert_cmpint (count, ==, -1);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT);
    }
  pair_clear (&pair);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/goodixtls/handshake/whole", GUINT_TO_POINTER (8192), test_handshake);
  g_test_add_data_func ("/goodixtls/handshake/fragmented", GUINT_TO_POINTER (7), test_handshake);
  g_test_add_data_func ("/goodixtls/handshake/bytewise", GUINT_TO_POINTER (1), test_handshake);
  g_test_add_func ("/goodixtls/handshake/wrong-key", test_wrong_key);
  g_test_add_func ("/goodixtls/handshake/abort-repeated", test_aborted_handshake);
  g_test_add_data_func ("/goodixtls/image/multiple-records", GINT_TO_POINTER (FALSE), test_image);
  g_test_add_data_func ("/goodixtls/image/corrupt-record", GINT_TO_POINTER (TRUE), test_image);
  return g_test_run ();
}
