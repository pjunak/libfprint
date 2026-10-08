See [Goodix validation](../doc/GOODIX_VALIDATION.md) for the current hardware matrix,
async transport/replay coverage, sanitizer/fuzz commands and recognition evaluator.
Use `-Dsigfm=enabled` to include the optional matcher/storage suite; the default
NBIS build has no C++/OpenCV requirement.

# Testing this fork

Run the hardware-free C/C++ suites from the repository root:

```sh
meson setup build -Ddrivers=all -Ddoc=false -Dintrospection=false \
  -Dinstalled-tests=false -Dgtk-examples=false
meson compile -C build
meson test -C build --print-errorlogs
```

`goodixtls` exercises a synthetic OpenSSL PSK client. `goodixtls-driver` exercises
command parsing, complete setup replay with a live TLS peer, USB error/cancellation
ownership and swipe completion without a reader. With Cairo, `goodixtls-capture`
executes the real public capture/enroll/verify/identify APIs and NBIS, mocking only
USB. It tests six-stage enrollment while holding the finger after movement stops,
matching a reloaded print on a recreated device, cancellation at eight lifecycle
points and recovery from protocol/cleanup failures. Its stripes come from a public
sample image, not a physical reader recording or an accuracy dataset.
`goodix-recognition` swipes six public fingerprint fixtures across a simulated
sensor with strong position-dependent shading and runs the driver's stripe
normalisation, stitching and NBIS: enrolled fingers must match themselves and
reject the others. It catches a return to edge-to-edge stacking, not accuracy.
`fpi-image-device` covers activation
recovery and enrollment updates. `capture-output` verifies private file creation,
pixel output and refusal to overwrite files/symlinks. `tools` covers trace import,
collection metadata and evaluation accounting. `fpi-print` covers template
save/load, malformed schemas and feature ownership. `pam-parallel` loads
`pam_fprint_parallel.so` through libpam against a scripted fprintd and logind on
a private D-Bus, typing into a pseudo-terminal; it needs `dbus-daemon` and
PyGObject and is skipped without them. `sigfm` checks stored
template bounds and round trips, including truncation at every byte boundary.
The optional `sigfm-legacy` tests require doctest. Virtual-device tests need
local Unix sockets; a restrictive sandbox may block them.

The GitHub workflow checks each commit with Goodix/virtual drivers, virtual-only
drivers, shared image helpers and every driver at once (upstream recorded-driver
fixtures and the `data/autosuspend.hwdb` check), under
sanitizers with bounded fuzzing, and builds the Arch package. It does not
publish packages.

Enable `-Dintrospection=true` for Python virtual-device and recorded-driver tests.
On Arch/CachyOS their additional dependencies are `gobject-introspection`,
`python-gobject`, `python-cairo` and `umockdev`. The fork's `-Ddrivers=all` selects
Goodix TLS plus virtual drivers; tests for other drivers are skipped intentionally.
The workflow's `every-driver` configuration selects all of them; `data/autosuspend.hwdb`
is only compared with the generated list in such a build
(`ninja -C build sync-udev-hwdb` regenerates it).

For a separate sanitizer build:

```sh
CC=clang meson setup build/asan -Ddrivers=all -Dsigfm=disabled -Ddoc=false \
  -Dintrospection=false -Dinstalled-tests=false -Db_sanitize=address,undefined \
  -Db_lundef=false
meson compile -C build/asan
ASAN_OPTIONS=detect_leaks=1 meson test -C build/asan --print-errorlogs
```

`tests/goodixtls-hardware-smoke.c` is a manual capture/cancellation test, never
run automatically. It requires USB access and leaves enrollment unchanged. Images
are discarded unless `--capture --output-dir` explicitly requests private PGM output.
[Validation instructions](../doc/GOODIX_VALIDATION.md) describe labeled collection,
finger-free USB recording and `import-goodix-trace.py`. The shipped Goodix TLS
setup fixture is synthetic; imported setup can be replayed through the same test.
The following upstream recording instructions require introspection and umockdev.

`umockdev` Tests
================
`umockdev` tests use fingerprint devices mocked by [`umockdev`
toolchain][umockdev].

This document describes how to create test cases (for USB devices). Many of
these tests are tests for image devices, where a single image is captured
and stored.

Other kinds of `umockdev` tests can be created in a similar manner. For
match-on-chip devices you would instead create a test specific `custom.py`
script, capture it and store the capture to `custom.pcapng`.

'capture' and 'custom' Test Creation
------------------------------------

For image devices the `capture.py` script will be used to capture one reference
image. If the driver is a non-image driver, then a `custom.py` script should be
created in advance, which will be run instead.

1. Make sure that libfprint is built with support for the device driver
   that you want to create a test case for.

2. From the build directory, run tests/create-driver-test.py as root. Note
   that if you're capturing data for a driver which already has a test case
   but the hardware is slightly different, you might want to pass a variant
   name as a command-line options, for example:
```sh
$ sudo tests/create-driver-test.py driver [variant]
```

3. If the capture is not successful, run the tool again to start another capture.

4. Add driver test name to `drivers_tests` in the `meson.build`, as instructed,
   and change the ownership of the just-created test directory in the source.

5. Check whether `meson test` passes with this new test.

**Note.** To avoid submitting a real fingerprint when creating a 'capture' test,
the side of finger, arm, or anything else producing an image with the device
can be used.


Possible Issues
---------------

Other changes may be needed to get everything working. For example the
`elan` driver relies on a timeout that is not reported correctly. In
this case the driver works around it by interpreting the protocol
error differently in the virtual environment (by means of
`FP_DEVICE_EMULATION` environment variable).


[umockdev]: https://github.com/martinpitt/umockdev
