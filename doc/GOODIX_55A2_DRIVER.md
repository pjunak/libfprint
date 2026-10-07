# The goodixtls55x4 driver (Goodix 27c6:55a2)

`goodixtls55x4` is a libfprint image driver for the Goodix `27c6:55a2` swipe
sensor. This page describes what the driver does, how it recovers from errors,
its limits, and what this fork changed. For installation see
[INSTALL_55a2.md](../INSTALL_55a2.md); for testing see
[GOODIX_VALIDATION.md](GOODIX_VALIDATION.md).

## The reader

- USB `27c6:55a2`, a Goodix sensor behind a Realtek MCU. Images arrive inside a
  TLS-PSK session negotiated over USB.
- Firmware `GF3206_RTSEC_APP_10052` and `GF3206_RTSEC_APP_10062` are accepted,
  both with the configuration the Windows driver uploads. Any other firmware
  is refused with `Unsupported Goodix firmware` instead of being guessed at.
- The reader must be paired with the key this driver expects. The driver reads
  the key's hash and refuses a different one (`Invalid device PSK`). It never
  writes keys or firmware.
- Frames are 56 × 176 pixels, 12 bits each; four rows and columns are cropped
  on every edge, leaving 48 × 168.

## Code layout

All files are in [libfprint/drivers/goodixtls](../libfprint/drivers/goodixtls).

| File | Responsibility |
| --- | --- |
| `goodix55x4.c` | Device lifecycle on libfprint's image-device API: activation, calibration, scanning, sleep, recovery |
| `goodix.c`, `goodix.h` | Commands, the USB read loop, matching ACKs and replies to the pending command, TLS handshake and image decryption |
| `goodixtls.c` | OpenSSL TLS-PSK server over memory BIOs |
| `goodix-transport.c` | Asynchronous USB writes with deadlines |
| `goodix_proto.c` | Packet and protocol framing and checksums (pure) |
| `goodix-profiles.c` | Supported firmware, geometry, configuration blob (pure) |
| `goodix-image.c` | Frame decoding, background subtraction, cropping, contrast (pure) |
| `goodix-swipe.c` | Swipe detection and stripe assembly (pure) |

The pure modules have no USB, timers or libfprint callbacks and are tested directly.

## Lifecycle

**Activation** (each verify, identify, capture or enrollment):

1. Configure: NOP, enable chip, read firmware version (selects the profile),
   check the paired key, reset the sensor, idle, upload configuration.
2. Negotiate a fresh TLS session, then wait 600 ms for the firmware to settle.
3. Calibrate: query MCU state, switch to finger-detection mode (the reply holds
   the detection baseline), wait until the sensor reports no finger, read one
   empty image as the background.
4. Report readiness to libfprint.

**Scan:** arm finger detection, then read frames at most 25 per second. A
frame whose mean drops a fixed margin below the background counts as contact. Distinct
frames during movement become stripes; the swipe ends when the finger is lifted
or stops moving. 12–60 stripes are stacked edge to edge into a 168-pixel-wide
image for NBIS. The image is handed over only after the finger has actually
left the sensor, so the next scan never calibrates against a finger.

**Enrollment** keeps one activation for all six swipes and reuses its calibration.

**Deactivation:** switch to idle, sensor sleep, MCU sleep. Each step is tried
even if an earlier one failed.

## Recovery

| Situation | Behaviour |
| --- | --- |
| Timeout, corrupt or stale packet, or lost TLS session during activation | Reinitialize once after 250 ms |
| Activation times out twice | Reset the USB port once per open, wait 2 s, reinitialize |
| Lost TLS session, dropped frame or timeout during a verify/identify swipe | Report "try again" (`FP_DEVICE_RETRY_GENERAL`); fprintd restarts with a fresh activation; at most twice per open |
| Finger resting on the sensor while finger detection is configured | If the sensor takes more than 300 ms to report empty, redo calibration once |
| Finger detection fires but nothing touches the sensor | After 50 empty frames (2 s), go back to waiting for a finger |
| Sleep command not acknowledged | Logged; the result already reported stands; the next activation starts from scratch |
| Unsupported firmware or a different paired key | Fail immediately, no retry |
| Reader behind a removable (external) USB port | Refuse to open it; see [built-in port check](#built-in-port-check) |
| Reader unplugged or re-enumerated (hibernate) | Current operation fails; the next one uses the new device |
| System suspend during a scan | libfprint cancels the operation; the next activation reinitializes |

Failures and recoveries are logged at message level, so they appear in
`journalctl -u fprintd` without enabling debug output.

Every command must be acknowledged within 1 s. Waiting for a finger to arrive
or leave has no deadline, but every wait can be cancelled.
Timers and the single outstanding USB read are owned explicitly: cancelling at
any point and immediately starting again cannot overlap two reads or run a
stale callback.

## Limits and inherited parameters

These values come from earlier forks and a small capture set. They are kept
until a representative, labeled capture corpus supports changing them (see
[offline recognition evaluation](GOODIX_VALIDATION.md#offline-recognition-evaluation)):

| Parameter | Value |
| --- | --- |
| NBIS bozorth3 match threshold | 24 |
| Stripes per swipe | 12 minimum, 60 maximum |
| Contact and release frame budgets | 700 each (empty frames do not count) |
| libfprint thermal model | hot after 30 min of activity, 9 min to cool |
| Contact threshold | 350 below the calibrated background mean |

False acceptance and rejection rates are not known. Stripes are stacked edge
to edge without estimating their overlap, so a slow swipe repeats ridge content
and a fast one skips some; NBIS then matches a stretched or compressed image.
`27c6:55b4` and `27c6:55a4` use related protocols but are not enabled; each
needs a validated firmware profile. The unrelated `goodixtls511` sources
(`27c6:5110`) were removed. The legacy SIGFM matcher is optional
(`-Dsigfm=enabled`) and off by default.

## Security of the USB link

The sensor and the computer talk through a TLS session, which normally does
two things: it keeps the images secret, and it proves that the messages come
from the real sensor. Both rely on a key that only the two ends know. Windows
pairs each reader with its own random key; Linux cannot obtain that key, so
the community tools pair the reader with a reference key instead. That key is
the same everywhere and is published in source code, and nobody has published
a way to give the sensor any other key. (Guessing a 256-bit key is not
feasible.)

With a public key, the link proves nothing. A small USB device programmed to
behave like the reader could send the computer a picture of the owner's
fingerprint, for example one lifted from a glass, and the driver would accept
it as a scan. Someone wired into the internal connection could also record
images. Both need physical access, special hardware and, for the first, a
usable copy of the fingerprint: a targeted attack, not a remote one.

### Built-in port check

The real reader is wired to an internal USB port. A look-alike would have to
come in through an external connector, and laptop firmware tells the kernel
which ports are external: the device's `removable` attribute in sysfs reads
`removable` for them and `fixed` for internal ones. The driver therefore
refuses to open a reader when its own port, or the port of any USB hub between
it and the computer, is `removable`. Checking the whole chain matters: a fake
hub plugged into an external port could otherwise present the fake reader on
an undescribed port of its own.

This turns the cheap version of the attack (plug in a gadget) into one that
requires opening the laptop and splicing into the internal cable. It does not
help when the firmware describes no ports (`unknown` everywhere); such readers
are accepted, because nothing distinguishes them. It does not protect against
someone who opens the machine. Check your laptop with:

```sh
cat /sys/bus/usb/devices/$(basename $(dirname $(grep -l 55a2 /sys/bus/usb/devices/*/idProduct)))/removable
```

`fixed` means the check protects you; `unknown` means it cannot.

If your reader genuinely sits behind an external port (for example a sensor
module on a USB adapter), the driver logs `Refusing a Goodix reader behind
removable USB port …` and fprintd cannot open it. Allow it explicitly in
fprintd's environment, which only root can change:

```ini
# /etc/systemd/system/fprintd.service.d/99-goodix-local.conf
[Service]
Environment=LD_LIBRARY_PATH=/opt/libfprint-goodix/lib
Environment=GOODIX_ALLOW_REMOVABLE_PORT=1
```

Treat the fingerprint as a convenience next to the password, not as a strong
second factor. Full-disk encryption is the best general protection against
someone with physical access, and a prerequisite for any future scheme that
keeps a secret key on the machine.

## Changes in this fork

Relative to the driver it started from:

- **Lifecycle:** motion stopping and finger release are separate states;
  replies are matched to the command type, so late images or unsolicited
  `0xc6` replies cannot complete another command; timers and the USB read have
  explicit owners; every wait is bounded or cancellable; cleanup runs even when
  a sleep command fails.
- **Robustness:** the recovery behaviours above, including one automatic USB
  reset for an unresponsive MCU (previously an opt-in environment option).
- **Validation:** firmware is checked against exact versions; configuration,
  detection baselines and frame lengths are validated; readers behind
  removable USB ports are refused (the TLS key is public, so the port is the
  only evidence that the reader is the built-in one).
- **Structure:** pure protocol, profile, image and swipe modules split out of
  the driver; environment options `GOODIX_USB_RESET`, `GOODIX_SWIPE_FDT` and
  `GOODIX_SAVE_DIR` removed (image collection is an explicit tool,
  `tools/collect-goodix.py`).
- **Core libfprint:** fixes to print serialization, activation-error state
  handling and timer finalization; SIGFM made optional so NBIS builds need no
  C++ or OpenCV.
- **Around the driver:** hardware-free test suites (simulated USB with a live
  TLS peer, public-API capture/enroll/verify), fuzzing, diagnostics and
  evaluation tools, an isolated Arch package, the
  [`pam_fprint_parallel`](PAM_FPRINT_PARALLEL.md) module and the
  `goodix-fingerprint-pam` setup tool.
