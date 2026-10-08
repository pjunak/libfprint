# Arch / CachyOS local package

From the repository, run `python tools/make-source-package.py /tmp/goodix-package`.
Review the generated `PKGBUILD`, then run `makepkg` in that directory as your normal
user. The archive includes current tracked and non-ignored untracked source files;
review `git status` first. The archive has normalized timestamps and ownership and
its SHA-256 is pinned in the recipe. The version contains both the Git revision
and a hash of the actual source contents, including local changes.

First run the [standalone readiness and capture checks](../../doc/GOODIX_VALIDATION.md#physical-acceptance-checks)
from a source build. They work with the installed fingerprint stack completely
removed and help separate driver failures from login/service configuration.

Install the resulting package with `sudo pacman -U <package.pkg.tar.zst>`.
It installs the library under `/opt/libfprint-goodix`, alongside the distribution
library. It does not replace the distribution's libfprint package, change PAM,
restart fprintd, or provision firmware/PSKs. No OpenCV dependency is needed.

The package also installs `pam_fprint_parallel.so` under
`/opt/libfprint-goodix/lib/security` (one prompt for password or finger; it talks
to fprintd over D-Bus). PAM is enabled separately with
`sudo goodix-fingerprint-pam install` (see
[INSTALL_55a2.md](../../INSTALL_55a2.md#4-enable-fingerprint-authentication)).
The package's pacman hook (`/usr/share/libalpm/hooks/goodix-fingerprint.hook`)
runs the read-only `goodix-fingerprint-pam check` after upgrades of the managed
PAM policies, `fprintd` or this package. It reports outdated overrides, a sudo
`.pacnew`, an `fprintd` that no longer resolves against this library, and an
older drop-in (such as `/opt/fprint55a2`'s `override.conf`) that still selects
another library.

The package also includes `goodix-collect`, `goodix-evaluate`, `goodix-import-trace`,
and the native capture/scoring helpers under `/opt/libfprint-goodix/libexec/libfprint-2`.
Those helpers load the library from the same isolated prefix. Capture collection
creates a private directory, records labels and build identity, and leaves enrollment
unchanged. See `doc/GOODIX_VALIDATION.md` beside the installed README (or
`../../doc/GOODIX_VALIDATION.md` relative to this file in the source checkout). Commands
using `build/` in that guide refer to a source build; installed collection/evaluation
commands select their packaged helpers by default.

## Activate and roll back

1. Keep the previous package file for rollback. Run `goodix-diagnose` and record
   existing fprintd drop-ins, particularly any `/opt/fprint55a2` override.
2. Put the included `fprintd-goodix.conf.example` at
   `/etc/systemd/system/fprintd.service.d/99-goodix-local.conf`. If that file
   already exists, back it up and review its contents first. Ensure no later
   drop-in overrides `LD_LIBRARY_PATH`.
3. Run `sudo systemctl daemon-reload` and `sudo systemctl restart fprintd` when
   no authentication operation is in progress. Test with `fprintd-verify` before
   relying on the login screen, then run `sudo goodix-fingerprint-pam install`.
4. To restore the previous package, use `sudo pacman -U <previous-package>` and
   restart fprintd. To restore the previous library selection, remove only the
   `99-goodix-local.conf` file you added (or restore its backup), reload systemd
   and restart fprintd. Existing unrelated drop-ins remain in effect.
5. If desired, run `sudo goodix-fingerprint-pam uninstall` and remove the isolated
   package with `sudo pacman -R libfprint-goodix-local`.

The package deliberately ships the service drop-in as an example. Library
installation is a package transaction; service selection is an explicit,
reversible administrative step. The recipe targets x86_64.
