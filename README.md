# libfprint with a Goodix 27c6:55a2 driver

A fork of [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) 1.94.6
that drives the **Goodix `27c6:55a2`** fingerprint reader, a small TLS-encrypted
swipe sensor that upstream libfprint does not support. It works with the normal
`fprintd` stack: enrollment, verification, the KDE lock screen, `sudo` and polkit
prompts.

> **Status: experimental.** It is used daily on one laptop, but recognition
> accuracy has only been checked on one person's fingers, and only two firmware
> versions are supported. Keep password login available.

> **Upgrading from a build before October 2026? Enroll your fingers again.**
> Older builds assembled swipe images incorrectly and could accept a finger
> that was not enrolled. The fixed driver will not match the old prints:
> `fprintd-delete $USER`, then `fprintd-enroll`.

## What works

| | |
| --- | --- |
| Reader | USB `27c6:55a2`, firmware `GF3206_RTSEC_APP_10052` or `GF3206_RTSEC_APP_10062` |
| fprintd | Enroll (6 swipes), verify, identify, saved prints across reboots |
| Lock screen | KDE `kde-fingerprint`; the driver reinitializes the reader after suspend and hibernate |
| sudo, pkexec, run0, KDE admin dialogs | One prompt that accepts the password **or** a finger, whichever comes first ([`pam_fprint_parallel`](doc/PAM_FPRINT_PARALLEL.md)) |
| Login screen | Password by default (it also unlocks KWallet and GNOME Keyring); fingerprint optional |

Not supported: other Goodix IDs such as `55b4` or `55a4` (they need their own
validated firmware profiles), other firmware versions, and replacement firmware.
This repository contains a userspace driver only.

## Install

On **Arch Linux / CachyOS**, build a package that installs next to the
distribution's libfprint without replacing it:

```sh
python tools/make-source-package.py /tmp/goodix-package
cd /tmp/goodix-package && makepkg
sudo pacman -U libfprint-goodix-local-*.pkg.tar.zst
```

Then point fprintd at it, enroll and enable the prompts. The steps, including
other distributions and rollback, are in **[INSTALL_55a2.md](INSTALL_55a2.md)**.

## Before you start

- **Sensor key.** The driver expects the reader to be paired with the key used
  by the Linux community tools. Windows (Windows Hello) has been observed to
  re-pair it; the driver then reports `Invalid device PSK`. The
  [restore procedure](INSTALL_55a2.md#invalid-device-psk-after-dual-booting-windows)
  writes the expected key back.
- **Swipe, don't press.** Place the finger at one end of the sensor and draw it
  slowly across the whole length in about a second. Short swipes are rejected.
- **Accuracy is barely measured.** Matching uses NBIS. On 28 swipes of one
  person's five fingers, no other finger came near the match threshold
  ([results](doc/GOODIX_VALIDATION.md#reference-results)), but that is one
  person and one session. Try a finger you have not enrolled after enrolling;
  [evaluation tools](doc/GOODIX_VALIDATION.md#offline-recognition-evaluation)
  exist, and results from more readers are welcome.
- **The sensor link is not secret.** Its TLS key is the public community key,
  so the link cannot prove that a scan comes from the real reader. The driver
  therefore only accepts a reader on a built-in USB port, which stops a gadget
  plugged into an external port but not someone who opens the laptop
  ([details](doc/GOODIX_55A2_DRIVER.md#security-of-the-usb-link)). Use the
  fingerprint for convenience, next to a good password.
- **Terminal `sudo` by fingerprint** cannot tell which prompt a swipe is meant
  for ([CVE-2024-37408](https://seclists.org/oss-sec/2024/q2/286)). Only swipe
  for a command you started.

## Documentation

- [INSTALL_55a2.md](INSTALL_55a2.md): installation, PAM setup, troubleshooting, uninstall
- [doc/GOODIX_55A2_DRIVER.md](doc/GOODIX_55A2_DRIVER.md): how the driver works, recovery behaviour, limits, changes in this fork
- [doc/PAM_FPRINT_PARALLEL.md](doc/PAM_FPRINT_PARALLEL.md): the combined password/fingerprint PAM module
- [doc/GOODIX_VALIDATION.md](doc/GOODIX_VALIDATION.md): automated tests, hardware checks, recognition evaluation
- [tests/README.md](tests/README.md): building and running the test suites
- [doc/UPSTREAM_MAINTENANCE.md](doc/UPSTREAM_MAINTENANCE.md): relationship to upstream libfprint

## Building and testing

Needs a C compiler, Meson, Ninja, pkg-config, GLib/GIO, libgusb and OpenSSL.
Cairo enables the end-to-end capture tests; libpam and libsystemd build the PAM
module.

```sh
meson setup build -Ddrivers=all -Ddoc=false -Dintrospection=false -Dinstalled-tests=false
meson compile -C build
meson test -C build --print-errorlogs
```

In this fork `-Ddrivers=default` builds `goodixtls55x4`; `all` adds the virtual
test drivers. Other upstream drivers can be listed by name. The tests need no
reader: USB is simulated, down to a TLS peer and a scripted fprintd.

## Reporting problems

Open an issue with the **Bug report** form; it asks for the output of
`goodix-diagnose` (reader, firmware, services and PAM, no biometric data) and
the fprintd journal, where the driver logs why an attempt failed. Use the
**Hardware report** form to tell us how the driver works on your laptop, even
if everything works. Never attach fingerprint images or enrolled prints.

Security problems: please report them privately, as described in
[SECURITY.md](SECURITY.md).

## Credits

This driver builds on the work of the
[goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev) project (Alexander
Meiler, Matthieu Charette and others), Ash and Natasha England-Elbro (SIGFM and
Goodix TLS), Alireza S.N. (the 55x4 driver) and RRieger
([Ravira43/libfprint](https://github.com/Ravira43/libfprint), the first working
`55a2` enrollment and verification). This fork reworks the driver's lifecycle
and error recovery, adds tests, Arch packaging and the PAM integration.

## License

LGPL 2.1 or later, like libfprint; see [COPYING](COPYING) and [AUTHORS](AUTHORS).
libfprint includes NIST NBIS code; see [README](README) for the upstream
notices. libfprint is part of the [fprint project](https://fprint.freedesktop.org/).
This fork is not an official libfprint release.
