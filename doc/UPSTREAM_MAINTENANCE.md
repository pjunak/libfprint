# Relationship to upstream libfprint

This fork is based on libfprint 1.94.6 and has not been rebased onto later
releases. Compared on 2026-09-23 against the official libfprint repository:

- Fork base: `v1.94.6`, commit `135a015b6a780e85f828a1bb9a62a2ee0c72e04b`.
- Compared release: `v1.94.10`, commit `0c97a47d8ef405cd577b87058c1e89cae9d242e7`.
- Official HEAD observed: `6f9479c3d55f847c1b3769f28ceb99227f9858cf`.
- This fork's base before the driver rework: `072991a`.

Source: [official v1.94.10 NEWS](https://gitlab.freedesktop.org/libfprint/libfprint/-/blob/v1.94.10/NEWS).
These are pinned comparison points, not an automatically updated latest-version claim.
No upstream branch has been merged/rebased into this working tree.

## Comparison findings

Upstream 1.94.7 raised the GLib requirement to 2.68 and simplified minutiae extraction.
1.94.9 moved the uru4000 crypto dependency to OpenSSL, added AppStream device metadata
and the `FP_DEVICE_RETRY_TOO_FAST` enum value. 1.94.10 contains additional device fixes,
including Goodix MOC and Synaptics; MOC is a different driver from this TLS sensor.
A wholesale source replacement would therefore mix build/API/device changes with
untested Goodix changes. The isolated package avoids replacing the system-wide library.
`pam_fprint_parallel` (in `pam/`) only talks to fprintd over D-Bus and is
independent of the libfprint version.

The current fork adds SIGFM hooks to the image/print core, additional Goodix TLS code,
and unrelated changes to other drivers. In particular, the wrong temporary-print
cleanup and SIGFM ownership/serialization problems are fork changes; do not report
those as upstream vulnerabilities. The new virtual-image callback wrappers and
state-machine callback tests can be reviewed separately from the Goodix implementation.
The exact firmware profile, pixel pipeline and USB writer are now separate internal
modules, and NBIS builds no longer require SIGFM/OpenCV. This reduces coupling when
porting the driver to a newer base.

Running the introspection suite exposed two fixes already represented in the pinned
upstream release: callback closure annotations and preserving an existing enrollment's
print type. The latter is adapted here to reject a mismatched SIGFM/NBIS format before
activation. They are local backports/adaptations, not new upstream findings. The MOC
recording test now explicitly performs its recorded initial template-list and clear
operations inside umockdev; it refuses to run on physical devices. This fork omits
upstream's automatic storage-check/reset path, and the test adjustment does not
reinstate automatic deletion in the driver.

## Review groups

Run `python tools/export-review-patches.py /tmp/libfprint-review` to export the complete
local delta against HEAD, including new files, without changing the index or history.
The manifest records the base and assigns each path exactly once:

1. Core image/print fixes and shared/virtual-device changes.
2. Goodix driver, protocol, transport, profiles and image processing.
3. Optional SIGFM implementation and storage/matcher fixes.
4. Build, tests, CI and installation metadata.
5. Diagnostic/evaluation tools and Arch package source generation.
6. Documentation.

Apply all nonempty patches in order to the recorded base with `git apply`. These are
review units, not independently buildable commits or patches ready to apply upstream;
build/test changes depend on the code groups.

For a future upstream port, use a separate checkout pinned to a selected release,
restore its build/CI defaults, and port only the required driver/core changes. Preserve
stored print IDs and public ABI; keep SIGFM opt-in. Run installed-header/introspection,
normal and sanitizer tests, then the [physical acceptance checks](GOODIX_VALIDATION.md#physical-acceptance-checks)
before switching a working system to the new library. General fixes are worth
submitting upstream separately, after checking whether upstream already has an
equivalent change.
