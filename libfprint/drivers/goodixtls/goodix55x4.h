// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>

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

#define GOODIX_55X4_INTERFACE (0)
#define GOODIX_55X4_EP_IN (0x82 | FPI_USB_ENDPOINT_IN)
#define GOODIX_55X4_EP_OUT (0x1 | FPI_USB_ENDPOINT_OUT)

#define GOODIX_55X4_PSK_FLAGS (0xbb020007)

/* Hash of the reference PSK provisioned by the Linux community tools (the
 * all-zero PMK, written through the white-box key), which the TLS server in
 * goodixtls.c uses. A reader paired by another driver reports a different hash
 * and is refused rather than re-provisioned. */
static const guint8 goodix_55x4_psk_0[] = {
  0x81, 0xb8, 0xff, 0x49, 0x06, 0x12, 0x02, 0x2a, 0x12, 0x1a, 0x94,
  0x49, 0xee, 0x3a, 0xad, 0x27, 0x92, 0xf3, 0x2b, 0x9f, 0x31, 0x41,
  0x18, 0x2c, 0xd0, 0x10, 0x19, 0x94, 0x5e, 0xe5, 0x03, 0x61
};

/* Only advertise devices with an explicit, tested-geometry profile. */
static const FpIdEntry id_table[] = {
  {.vid = 0x27c6, .pid = 0x55a2},
  {.vid = 0, .pid = 0, .driver_data = 0},
};
