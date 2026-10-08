# Security policy

## Reporting a vulnerability

Please report security problems privately: open this repository's
**Security** tab on GitHub and choose **Report a vulnerability**. Do not open a
public issue for them.

Include what is affected, the version (`pacman -Q libfprint-goodix-local` or
`git describe --always`), how to reproduce it, and what an attacker gains. This
is a volunteer project; reports are handled as time allows, and fixes go to
the `master` branch.

## In scope

- **`pam_fprint_parallel`** (`pam/`): anything that yields `PAM_SUCCESS`
  without a matching finger, skips the password check, bypasses
  `pam_faillock`, or exposes or keeps a typed password.
- **`goodix-fingerprint-pam`** (`tools/goodix-fingerprint-pam.py`): it runs as
  root and writes `/etc/pam.d`.
- **The `goodixtls55x4` driver**: memory-safety or logic errors when parsing
  data from the USB device.
- **Packaging**: file permissions, the pacman hook.

## Known limitations

These are documented design limits, not vulnerabilities:

- The sensor's TLS key is the public key used by the community tools, so the
  USB link cannot prove that a scan comes from the real reader. The driver only
  accepts a reader on a built-in port; see
  [Security of the USB link](doc/GOODIX_55A2_DRIVER.md#security-of-the-usb-link).
- A fingerprint prompt in a terminal cannot show which process is asking
  ([CVE-2024-37408](https://seclists.org/oss-sec/2024/q2/286)); see
  [pam_fprint_parallel](doc/PAM_FPRINT_PARALLEL.md#security-notes).
- Recognition accuracy (false acceptance and rejection rates) has only been
  checked on one person's fingers in one session
  ([results](doc/GOODIX_VALIDATION.md#reference-results)). A finger that is
  not enrolled being accepted is a vulnerability: please report it.
- Someone who opens the machine and splices into the reader's internal
  connection is not stopped.

Problems in upstream [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint)
or [fprintd](https://gitlab.freedesktop.org/libfprint/fprintd) belong to those
projects.
