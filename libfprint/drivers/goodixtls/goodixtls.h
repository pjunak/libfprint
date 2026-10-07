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

#pragma once

#include <gio/gio.h>
#include <openssl/ssl.h>

/* All TLS processing runs on the device's main context. Memory BIOs let USB
 * callbacks supply records without blocking on a socket or a worker thread. */
typedef struct
{
  SSL_CTX *ssl_ctx;
  SSL     *ssl_layer;
} GoodixTlsServer;

gboolean goodix_tls_server_init (GoodixTlsServer *self,
                                 GError         **error);
gboolean goodix_tls_server_deinit (GoodixTlsServer *self,
                                   GError         **error);
gboolean goodix_tls_server_handshake (GoodixTlsServer *self,
                                      gboolean        *complete,
                                      GError         **error);
int goodix_tls_client_send (GoodixTlsServer *self,
                            const guint8    *data,
                            guint16          length);
int goodix_tls_client_recv (GoodixTlsServer *self,
                            guint8          *data,
                            guint16          length);
int goodix_tls_server_receive (GoodixTlsServer *self,
                               guint8          *data,
                               guint32          length,
                               GError         **error);
