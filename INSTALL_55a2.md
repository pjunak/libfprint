# Installing the Goodix `27c6:55a2` fingerprint driver

This guide sets up the `goodixtls55x4` driver for the **Goodix `27c6:55a2`**
sensor so you can authenticate with your finger through `fprintd` + PAM. The
recommended setup types the password once after a cold boot, which also unlocks
KWallet and GNOME Keyring, and uses the finger for everything after that:

| Prompt | PAM service | Behaviour after setup |
| --- | --- | --- |
| Login after boot (Plasma Login Manager) | `plasmalogin` | Password (unlocks the wallets). Fingerprint is [opt-in](#fingerprint-at-the-login-screen-opt-in) |
| Lock screen, including after suspend/hibernate | `kde-fingerprint` | Swipe at any time; the password field works in parallel |
| `sudo` in a terminal | `sudo` | One prompt: type the password **or** swipe, whichever comes first; no timeout |
| `pkexec`, `run0`, KDE administrator dialogs | `polkit-1` | The same, in the dialog |

The repository contains a userspace driver and configuration blob, not a
replacement firmware image. Recognition accuracy and the false-acceptance rate
of this small swipe sensor have not been measured; keep password login
available. Background: [driver](doc/GOODIX_55A2_DRIVER.md),
[PAM module](doc/PAM_FPRINT_PARALLEL.md), [testing](doc/GOODIX_VALIDATION.md).

---

## Arch / CachyOS (recommended)

The driver is packaged as `libfprint-goodix-local`, installed under
`/opt/libfprint-goodix` next to the distribution's libfprint. Only `fprintd`
loads it, through a systemd drop-in. See [packaging/arch/README.md](packaging/arch/README.md)
for package details and rollback.

### 1. Build and install the package

```bash
sudo pacman -S --needed base-devel git meson ninja glib2 libgusb openssl cairo \
  dbus python-gobject fprintd
git clone https://github.com/pjunak/libfprint.git
cd libfprint
python tools/make-source-package.py /tmp/goodix-package
cd /tmp/goodix-package && makepkg
sudo pacman -U /tmp/goodix-package/libfprint-goodix-local-*.pkg.tar.zst
```

`makepkg` runs the hardware-free test suites. The package version contains the
Git revision and a hash of the source, so local changes are visible in it. Keep
the previous package file for rollback.

### 2. Point fprintd at the isolated library (once)

```bash
sudo install -Dm644 /usr/share/doc/libfprint-goodix-local/fprintd-goodix.conf.example \
  /etc/systemd/system/fprintd.service.d/99-goodix-local.conf
sudo systemctl daemon-reload
sudo systemctl restart fprintd
```

Check `systemctl cat fprintd` for other drop-ins that also set `LD_LIBRARY_PATH`.
The isolated library contains only the Goodix driver: with the drop-in active,
fprintd no longer sees other fingerprint readers on the same machine.
After a package upgrade, `sudo systemctl restart fprintd` (or a reboot) loads
the new library; fprintd also exits on its own when idle, so the next
authentication starts it with the new library.

### 3. Enroll a finger

Enroll as **your own user** (no `sudo`, or it enrolls `root`):

```bash
fprintd-enroll
fprintd-verify
```

Swipe 6 times until `enroll-completed`. Enrolling a second finger
(`fprintd-enroll -f left-index-finger`) gives you a fallback; PAM accepts any
enrolled finger.

### 4. Enable fingerprint authentication

```bash
goodix-fingerprint-pam diff            # review the changes
sudo goodix-fingerprint-pam install
goodix-fingerprint-pam status
```

(From a source checkout without the package: `sudo python3 tools/goodix-fingerprint-pam.py install`.)

By default this enables `sudo` and `polkit-1`, using the package's
`pam_fprint_parallel.so` (see [below](#how-sudo-and-administrator-prompts-work)).
The tool writes `/etc/pam.d/polkit-1`
as a copy of the distribution policy in `/usr/lib/pam.d` with one marked block
inserted, and inserts the same kind of block into `/etc/pam.d/sudo`. Replaced
files are backed up in `/var/lib/goodix-fingerprint-pam/backup/`. Keep a
logged-in terminal open while you test; no service restart is needed.

Every fingerprint step comes after the shell, `nologin` and `faillock` checks,
so it cannot bypass an account lockout. Fingerprints are not offered over SSH.
`su` is deliberately left alone: it authenticates the *target* user (root), who
has no enrolled finger. The tool knows Plasma Login Manager; for SDDM or GDM,
see [PAM on other systems](#pam-on-other-systems).

The package installs a pacman hook that runs `goodix-fingerprint-pam check`
after upgrades to these PAM policies, `fprintd` or the driver. It warns when a
distribution policy changed underneath an override (it names the
`install <service>` command that regenerates it), when `sudo` got a `.pacnew`,
or when `fprintd` no longer runs against the isolated library.

To remove the fingerprint steps again: `sudo goodix-fingerprint-pam uninstall`
(or name a service). Overrides that become identical to the distribution policy
are deleted.

### How sudo and administrator prompts work

`pam_fprint_parallel.so` shows one prompt, `[sudo] password or fingerprint for USER:`,
and accepts whichever comes first:

- **A matching finger** → authenticated.
- **A typed password + Enter** → handed to the usual `pam_unix` check; a wrong
  password gets sudo's normal "Sorry, try again".
- **Enter on an empty line** → ignored while the reader is armed.
- **Non-matching fingers** → after three, only the password is accepted.
- **No timeout** → the prompt waits for one or the other.
- **Ctrl+C** → cancels sudo as usual; the terminal is restored.

The module never checks passwords itself. It steps aside, leaving sudo's normal
prompt, over SSH, for piped input (`sudo -S`, `cat file | sudo tee …`), and when
no finger is enrolled or fprintd is unavailable. Administrator dialogs (polkit)
accept either as well. KDE's dialog keeps its "Password…" field but shows
"Swipe your finger or type your password" above it, and "not recognized" or
"too short" messages while it waits.

The reader serves one client at a time. While a prompt waits, the module
releases the reader whenever the screen locks, so the lock screen can use it,
and takes it back after unlocking. Another prompt that finds the reader busy
retries every two seconds.

Fingerprint `sudo` in a terminal cannot tell *which* prompt a swipe is meant
for: a `sudo` waiting unseen in another terminal or script can take a swipe
intended for something else ([CVE-2024-37408](https://seclists.org/oss-sec/2024/q2/286)).
Only swipe when you started the command that asks. To remove the fingerprint
from sudo: `sudo goodix-fingerprint-pam uninstall sudo`.

### Fingerprint at the login screen (opt-in)

KWallet and GNOME Keyring are encrypted with your login password and are
unlocked by the password you type at the login screen. A fingerprint login
supplies no password, so both ask for it separately after login. Waking from
suspend or hibernate goes to the lock screen, where the wallets are already
open; the password is therefore needed only after a cold boot.

If you prefer the finger there anyway: `sudo goodix-fingerprint-pam install plasmalogin`.
Plasma Login Manager runs PAM after you press Enter, one step at a time:

- **Password typed** → checked first; you log in immediately and the wallets open.
- **Empty password + Enter** → one fingerprint attempt (two swipes, 15 s).
- **Wrong password** → you may also swipe, or wait up to 15 s to retry.

Removing the wallet prompts would mean giving the wallets an empty password or
storing the login password for the fingerprint to release. Neither is advisable
without full-disk encryption and Secure Boot.

### How the lock screen works

KDE's lock screen runs the password and `kde-fingerprint` stacks in parallel.
A fingerprint attempt lasts 30 s (three swipes). Afterwards, or after a failed
attempt, it restarts when the lock screen UI appears again: move the mouse or
press a key. After four attempts that each fail within two seconds, the lock
screen disables fingerprint until the next lock; the driver now retries
transient sensor errors itself, so this should only happen with a broken reader.

On suspend, libfprint cancels a running verification; on resume (and after
hibernation, which re-enumerates the USB device) the next attempt
reinitializes the sensor completely.

---

## Other distributions (manual build)

### Build dependencies

Debian / Ubuntu (as used by the CI):

```bash
sudo apt install build-essential git meson ninja-build pkg-config libglib2.0-dev \
  libgusb-dev libssl-dev libcairo2-dev libpam0g-dev libsystemd-dev fprintd libpam-fprintd
```

openSUSE:

```bash
sudo zypper install -t pattern devel_basis
sudo zypper install meson ninja gcc git pkg-config glib2-devel libgusb-devel \
  openssl-devel cairo-devel pam-devel systemd-devel fprintd fprintd-pam
```

SIGFM is disabled by default; the 55a2 uses NBIS and needs neither OpenCV nor a
C++ compiler. Legacy SIGFM enrollments need `-Dsigfm=enabled`, OpenCV 4 or 5
and a C++17 compiler; NBIS-only builds reject them explicitly.

### Build and test standalone

```bash
meson setup build -Ddrivers=goodixtls55x4 -Ddoc=false -Dintrospection=false \
  -Dinstalled-tests=false -Dgtk-examples=false
ninja -C build
# Keep the sensor clear: later attempts must finish calibration before cancelling.
sudo timeout 30s build/tests/goodixtls-hardware-smoke --cycles=2
# Wait for Ready, then swipe and lift; two samples per cycle, images discarded.
sudo timeout 120s build/tests/goodixtls-hardware-smoke --capture --cycles=2 --delay-ms=15000
```

Stop fprintd while running the standalone helper. It does not enroll or save
images. See [validation](doc/GOODIX_VALIDATION.md) and [tests/README.md](tests/README.md).

### Install the library in an isolated directory

```bash
sudo systemctl stop fprintd
sudo install -d /opt/fprint55a2/lib
sudo install -m0755 build/libfprint/libfprint-2.so.2.0.0 /opt/fprint55a2/lib/
sudo ln -sf libfprint-2.so.2.0.0 /opt/fprint55a2/lib/libfprint-2.so.2
sudo install -Dm644 /dev/stdin /etc/systemd/system/fprintd.service.d/goodix55a2.conf <<'EOF'
[Service]
Environment=LD_LIBRARY_PATH=/opt/fprint55a2/lib
EOF
sudo systemctl daemon-reload
```

`/opt` is not in `ld.so.conf`, so only fprintd picks up this library. Confirm
with `sudo grep fprint55a2 /proc/$(pgrep -x fprintd)/maps` after `fprintd-list "$USER"`.

### PAM on other systems

`goodix-fingerprint-pam` knows the Arch/CachyOS policy layout and Plasma Login
Manager. Elsewhere, edit the specific services you want (`sudo`, polkit, your
display manager): put the fingerprint line after that service's account and
lockout checks and before its password stack. Either use
`auth sufficient pam_fprintd.so`, or install `build/pam/pam_fprint_parallel.so`
and use the lines from [its documentation](doc/PAM_FPRINT_PARALLEL.md#configuration).
**Do not add it to a shared stack such as `common-auth` or `system-auth`**:
`sshd` and `su` include those too.

---

## Swipe technique (matters a lot)

The sensor is tiny and treated as a **swipe** sensor:

- Place the finger flat at **one end** of the sensor, light, even pressure.
- Move **slowly and continuously** across the whole long axis (~1–1.5 s).
- **Lift only at the other end.**
- A short swipe reports `too-short` (wording depends on the application); retry.
- Wait until the prompt appears before touching the sensor: initialization
  calibrates on an empty sensor. A resting finger delays readiness until it is lifted.

---

## Troubleshooting

Start with `goodix-fingerprint-pam status` and `journalctl -b -u fprintd`.
The driver logs why an attempt failed (initialization, lost TLS session,
missed sleep, port reset, long wait for an empty sensor) without debug logging.

### "Unsupported Goodix firmware …"

The driver accepts exactly `GF3206_RTSEC_APP_10052` and `GF3206_RTSEC_APP_10062`
with the original 55a2 configuration. Another version needs a validated profile.

### "Invalid device PSK" after dual-booting Windows

Windows (Windows Hello) has been observed to re-pair the sensor with a new key.
Avoid enrolling or using the reader in Windows. To restore the key this driver
expects, the original recovery procedure uses an external
[restore script](https://github.com/Ravira43/goodix-fp-dump/blob/master/restore_psk_55a2.py).
It writes persistent sensor key material; inspect it and confirm the USB model first:

```bash
git clone https://github.com/Ravira43/goodix-fp-dump.git
cd goodix-fp-dump
python3 -m venv .venv
.venv/bin/pip install pyusb
sudo systemctl stop fprintd
sudo .venv/bin/python restore_psk_55a2.py   # writes PSK_WHITE_BOX, verifies hash
sudo systemctl start fprintd
```

This error is not retried; it fails each attempt immediately.

### The sensor stops detecting the finger / every attempt times out

When two initializations in a row get no answer, the driver resets the USB
port once per attempt and reinitializes (journal: "resetting its USB port
once"). If attempts still fail, check that no other program holds the reader,
then reboot. A manual reset (requires PyUSB) is:

```bash
sudo systemctl stop fprintd
sudo python3 -c "import usb.core,time; d=usb.core.find(idVendor=0x27c6,idProduct=0x55a2); d.reset(); time.sleep(2)"
sudo systemctl start fprintd
```

### "Device was already claimed"

Finish any enrollment or verification in other applications first. If nothing
is running, `sudo systemctl restart fprintd`.

### The LED never turns orange

No `SetLed` (`0xc6`) payload has been found that changes the LED on this
firmware. LED colour is not a readiness indicator.

### `GOODIX_USB_RESET`, `GOODIX_SWIPE_FDT`, `GOODIX_SAVE_DIR`

Retired; they have no effect. Remove them from old service configuration. For
private capture collection use `tools/collect-goodix.py` as described in the
[validation guide](doc/GOODIX_VALIDATION.md).

For a read-only report of device, service and PAM state:
`python tools/goodix-diagnose.py`. For driver debug output, run the standalone
helper with `G_MESSAGES_DEBUG=all`.

---

## Uninstall

```bash
sudo goodix-fingerprint-pam uninstall
fprintd-delete "$USER"        # optional, while the driver still works
sudo rm /etc/systemd/system/fprintd.service.d/99-goodix-local.conf
sudo systemctl daemon-reload && sudo systemctl restart fprintd
sudo pacman -R libfprint-goodix-local
```

For a manual installation, remove `/etc/systemd/system/fprintd.service.d/goodix55a2.conf`
and `/opt/fprint55a2` instead of the package. Unrelated drop-ins stay in effect.
