// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "goodixtls.h"

#include <openssl/err.h>
#include <string.h>

static void
set_tls_error (GError **error, const char *operation, int ssl_error)
{
  unsigned long code = ERR_peek_last_error ();
  const char *reason = code ? ERR_reason_error_string (code) : NULL;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
               "%s failed (SSL error %d): %s", operation, ssl_error,
               reason ? reason : "connection closed or invalid TLS data");
}

static unsigned int
tls_server_psk_callback (SSL *ssl, const char *identity,
                         unsigned char *psk, unsigned int max_psk_len)
{
  /* The provisioned white-box key corresponds to a 32-byte zero PSK. */
  if (max_psk_len < 32)
    return 0;
  memset (psk, 0, 32);
  return 32;
}

gboolean
goodix_tls_server_deinit (GoodixTlsServer *self, GError **error)
{
  /* No SSL_shutdown: the USB sleep/reset command ends the sensor session.
   * Waiting for close_notify here would hang when the sensor has gone away. */
  g_clear_pointer (&self->ssl_layer, SSL_free);
  g_clear_pointer (&self->ssl_ctx, SSL_CTX_free);
  return TRUE;
}

gboolean
goodix_tls_server_init (GoodixTlsServer *self, GError **error)
{
  BIO *input = NULL;
  BIO *output = NULL;

  ERR_clear_error ();
  self->ssl_ctx = SSL_CTX_new (TLS_server_method ());
  if (!self->ssl_ctx)
    goto fail;

  /* Keep the cipher policy required by the GF3206 firmware, scoped to this
   * USB transport. The device supports TLS 1.2 with legacy PSK ciphers. */
  SSL_CTX_set_security_level (self->ssl_ctx, 0);
  SSL_CTX_set_dh_auto (self->ssl_ctx, 1);
  SSL_CTX_set_psk_server_callback (self->ssl_ctx, tls_server_psk_callback);
  if (!SSL_CTX_set_cipher_list (self->ssl_ctx, "PSK:@SECLEVEL=0") ||
      !SSL_CTX_set_min_proto_version (self->ssl_ctx, TLS1_2_VERSION) ||
      !SSL_CTX_set_max_proto_version (self->ssl_ctx, TLS1_2_VERSION))
    goto fail;

  self->ssl_layer = SSL_new (self->ssl_ctx);
  input = BIO_new (BIO_s_mem ());
  output = BIO_new (BIO_s_mem ());
  if (!self->ssl_layer || !input || !output)
    goto fail;

  BIO_set_mem_eof_return (input, -1);
  BIO_set_mem_eof_return (output, -1);
  SSL_set_bio (self->ssl_layer, input, output); /* transfers both BIOs */
  SSL_set_accept_state (self->ssl_layer);
  return TRUE;

fail:
  set_tls_error (error, "TLS initialization", SSL_ERROR_SSL);
  BIO_free (input);
  BIO_free (output);
  goodix_tls_server_deinit (self, NULL);
  return FALSE;
}

int
goodix_tls_client_send (GoodixTlsServer *self, const guint8 *data, guint16 length)
{
  return BIO_write (SSL_get_rbio (self->ssl_layer), data, length);
}

int
goodix_tls_client_recv (GoodixTlsServer *self, guint8 *data, guint16 length)
{
  BIO *output = SSL_get_wbio (self->ssl_layer);

  if (!BIO_ctrl_pending (output))
    return 0;
  return BIO_read (output, data, length);
}

gboolean
goodix_tls_server_handshake (GoodixTlsServer *self,
                             gboolean *complete, GError **error)
{
  int result;
  int ssl_error;

  *complete = FALSE;
  ERR_clear_error ();
  result = SSL_do_handshake (self->ssl_layer);
  if (result == 1)
    {
      *complete = TRUE;
      return TRUE;
    }

  ssl_error = SSL_get_error (self->ssl_layer, result);
  if (ssl_error == SSL_ERROR_WANT_READ)
    return TRUE; /* resume when the next USB TLS packet arrives */

  set_tls_error (error, "TLS handshake", ssl_error);
  return FALSE;
}

int
goodix_tls_server_receive (GoodixTlsServer *self, guint8 *data,
                           guint32 length, GError **error)
{
  guint32 total = 0;

  /* A single USB image packet can contain several TLS application records. */
  while (total < length)
    {
      int result;
      int ssl_error;

      ERR_clear_error ();
      result = SSL_read (self->ssl_layer, data + total,
                         MIN (length - total, G_MAXINT));
      if (result > 0)
        {
          total += result;
          continue;
        }

      ssl_error = SSL_get_error (self->ssl_layer, result);
      if (ssl_error == SSL_ERROR_WANT_READ)
        {
          if (total)
            return total;
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
                               "Incomplete TLS image record");
        }
      else
        set_tls_error (error, "TLS image read", ssl_error);
      return -1;
    }

  return total;
}
