# Goodix validation and recognition evaluation

## What is automated

`meson test` covers TLS with a synthetic OpenSSL peer, driver completion/cancellation,
fragmented/coalesced wire packets, explicit firmware profiles, image conversion,
print storage and optional SIGFM matching. With Cairo, `goodixtls-capture` runs
the real public capture, enrollment, verification and identification APIs. It
checks six enrollment scans with a held finger, reloads the serialized print on
a recreated device, and verifies recovery after cancellations and protocol errors.
The image source is a public sample print transformed into synthetic sensor stripes;
it is not a physical swipe recording or recognition-quality measurement.
`goodix-recognition` swipes other drivers' public test captures across a
simulated sensor with a strong position-dependent response, through the
driver's normalisation, stitching and NBIS. Two enrolled fingers must match
themselves and reject the four others. It fails for the old edge-to-edge
pipeline and for stitching without normalisation, but it does not measure
accuracy.
`goodix-wire-replay.ini` is a **synthetic** firmware-command fixture.
It exposed uninitialized reserved command bytes under GCC;
it is not a recording from a physical reader and does not establish USB timing.
`goodix-setup-replay.ini` covers the complete cleartext initialization. Its bytes
are checked against asynchronous USB writes, including the multi-transfer config
upload. A live test TLS client then negotiates with the driver and sends an encrypted
image after calibration. The test repeats after cancelling a pending read, using
whole and seven-byte USB fragments. Both checked-in fixtures are synthetic and
contain no biometrics.

The GitHub workflow builds NBIS with GCC and Clang, optional SIGFM with distribution
OpenCV, installed C/C++ header consumers, GObject introspection and Arch packaging.
Its sanitizer job runs the suite with ASan/UBSan/LSan and bounded fuzzing. Configure
an equivalent local fuzz build with:

```sh
CC=clang meson setup /tmp/goodix-fuzz -Ddrivers=all -Dsigfm=disabled \
  -Ddoc=false -Dintrospection=false -Dinstalled-tests=false -Dfuzzing=true \
  -Db_sanitize=address,undefined -Db_lundef=false \
  -Dc_args=-fsanitize=fuzzer-no-link
meson compile -C /tmp/goodix-fuzz
python tools/make-fuzz-corpus.py /tmp/goodix-corpus
/tmp/goodix-fuzz/tests/generate-print-seeds /tmp/goodix-corpus/print
/tmp/goodix-fuzz/tests/fuzz-goodix /tmp/goodix-corpus/goodix -max_total_time=30 -max_len=65539
/tmp/goodix-fuzz/tests/fuzz-print /tmp/goodix-corpus/print -max_total_time=30 -max_len=65536
```

Clang's sanitizer runtime is resolved by the executable, so `b_lundef=false` is
needed for instrumented shared libraries. See [LibFuzzer documentation](https://llvm.org/docs/LibFuzzer.html).

## Physical acceptance checks

Keep a password login available. Perform these checks locally; an automated test
must not suspend/reboot the host or change another user's enrollment.

1. Run `python tools/goodix-diagnose.py > diagnosis.json` before changing service
   configuration. A null field means unavailable, not success or absence. The
   tool reads PAM auth includes, sysfs and filtered service information; it never
   claims the USB device or reads fingerprint images/templates. The reported
   `profile` identifies the USB model and `supported_firmware` lists the versions
   allowed by the tool's build. Neither measures the reader's firmware. Only
   `last_firmware`, when available, comes from a device response recorded in the
   journal, including firmware-rejection errors.
2. The standalone binary loads its build-tree library; no installation, fprintd or
   PAM configuration is required. If fprintd is installed/running, stop it while
   testing so two processes do not claim the same USB interface. Record its initial
   state and restore it afterward. Use the build directory supplied with your build
   in place of `build` below. Start with a short check, sensor clear:
   `sudo timeout 30s build/tests/goodixtls-hardware-smoke --cycles=2`.
3. Once the short check passes, run the lifecycle soak (100 open/close cycles,
   two immediate attempts per cycle):
   `sudo timeout 1200s build/tests/goodixtls-hardware-smoke --cycles=100 --scenario=steady > lifecycle.jsonl`.
   Do not touch the sensor in cancellation mode. The first attempt cancels during
   initialization; subsequent attempts allow three seconds and must reach readiness
   before cancellation. This prevents a stuck initialization from passing the soak.
   Exit 124 means a watchdog detected an operation that did not finish. Other nonzero
   exits mean a failed check. Each completed attempt reports `ready`, `finger_seen`,
   `passed` and timing as JSON, with no image data.
4. Test captures with `sudo timeout 120s build/tests/goodixtls-hardware-smoke --capture --cycles=2 --delay-ms=15000`.
   Keep the sensor clear until the helper prints `Ready`, then swipe and lift.
   Each cycle requests two swipes. Images are immediately discarded. `ready=false`
   isolates a setup/calibration failure; `ready=true` with `finger_seen=false` means
   no finger was detected. This checks capture, not recognition quality or persistence.
5. After standalone checks pass, install/activate the isolated library if needed,
   restore fprintd and run `fprintd-verify`. Test a new enrollment on a dedicated
   test account, then successful verification, a different finger, cancellation,
   immediate retry and lock-screen unlock. Do not replace your only working enrollment
   just to run a test. The library never writes the sensor's firmware/PSK in these tests.
6. Repeat after a Linux warm reboot, a shutdown/power-on, and suspend/resume; use
   `--scenario=warm-boot`, `cold-boot` or `resume` to label standalone logs.
   Make capture the first fingerprint operation after boot so the initial setup
   is tested directly, before a cancellation/retry has initialized the sensor.
   Test the login screen **before first password login**, then password fallback,
   lock-screen unlock and sudo. The login PAM setup is separate from sensor discovery.

Record each run in a table like this one; write "pending" for anything not tested:

| Build | USB / firmware | Scenario | Cancel + immediate retry | Capture | Enroll + verify | Login | Password fallback |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `pkgver` or `git describe` | 27c6:55a2 / GF3206_RTSEC_APP_100xx | steady, warm-boot, cold-boot, resume or 100 cycles | | | | | |

Reference results so far come from one laptop (27c6:55a2, firmware
`GF3206_RTSEC_APP_10052`, Arch-based CachyOS, fprintd 1.94.5, KDE Plasma 6.7):
standalone readiness/cancellation and capture checks, including the first
capture after a warm reboot; six-stage enrollment through fprintd; saved-print
verification after reboots; and everyday use at the login screen, in `sudo`
and in polkit dialogs. Too-short swipes are rejected and retried as expected.
Not yet done: the 100-cycle soak, cold-boot and resume runs of the standalone
helper, and any measurement of recognition accuracy. Reports from other
readers are welcome; include the build version and firmware.

### Authentication acceptance (after `goodix-fingerprint-pam install`)

Keep a logged-in terminal open. After each step, `journalctl -b -u fprintd`
shows any initialization failure, retry, missed sleep or port reset.

1. Reboot and type the password at the login screen: immediate login, no
   KWallet or keyring prompt. (Only if `plasmalogin` was enabled: log out, press Enter on an
   empty password and swipe; expect the two wallet prompts.)
2. Lock with Meta+L and swipe. Lock, wait 40 s, move the mouse, swipe (re-arm
   after the 30 s attempt). Unlock with the password while the reader waits.
3. Hibernate, resume, swipe at the lock screen. Repeat with `systemctl suspend`
   while the lock screen is waiting for a finger.
4. `sudo -k; sudo true`: swipe. Again: type the password instead. Again: Enter
   on an empty line (ignored), wait a minute (no timeout), then swipe. Three
   wrong fingers, then the password. Ctrl+C: terminal echoes normally after.
   `echo | sudo -S true` and over SSH: sudo's own password prompt only.
5. `pkexec true` and a KDE administrator dialog: swipe; again with the password.
6. Leave a `sudo` prompt waiting, lock the screen (Meta+L), unlock with the
   finger, then answer the waiting prompt with the finger.
7. Rest the finger on the sensor before a prompt appears, then lift and swipe:
   the attempt must still detect the swipe (detection re-baseline).
8. Try a finger that is not enrolled at each prompt: no unlock.

The 700-contact-frame and 700-release-frame budgets, at most 25 frames/s, are not
measured thermal limits. Empty waiting frames do not spend the contact budget.
The inherited thermal-model constants remain uncalibrated. The old
`GOODIX_SWIPE_FDT`, `GOODIX_USB_RESET` and `GOODIX_SAVE_DIR` options were removed.

## Physical USB replay

The Goodix replay uses recorded cleartext setup followed by a fresh synthetic TLS
session. It does not reuse captured ciphertext, weaken production randomness, or
claim to reproduce USB timing. The native test replaces USB submission only in its
own binary. `GOODIX_TEST_SETUP_TRACE` is read by the test, never by the library.

To record setup, install `tshark`, enable Linux usbmon (`sudo modprobe usbmon`),
and find the current bus/address with `lsusb -d 27c6:55a2`. Stop fprintd as described
above. Use an owned private directory outside the checkout. In one terminal record
only the selected device's fields (replace bus 3/address 7 with your current values):

```sh
umask 077
mkdir -m 700 /tmp/goodix-recording
sudo tshark -n -l -i usbmon3 -s 0 -a duration:20 \
  -Y 'usb.bus_id == 3 && usb.device_address == 7' \
  -T fields -E occurrence=f \
  -e usb.bus_id -e usb.device_address -e usb.endpoint_address \
  -e usb.urb_type -e usb.urb_status -e usb.data_len -e usb.capdata \
  > /tmp/goodix-recording/setup.tsv
```

While it records, run in another terminal without touching the reader:

```sh
sudo timeout 30s build/tests/goodixtls-hardware-smoke \
  --cycles=1 --initial-cancel-ms=5000 --delay-ms=5000
```

The longer first deadline allows initialization to reach TLS before cancelling.
Restore fprintd afterward. Import the recording, recording the source revision used
to build the binary, then run it through the driver:

```sh
python tools/import-goodix-trace.py /tmp/goodix-recording/setup.tsv \
  /tmp/goodix-recording/setup.ini --tsv --bus=3 --address=7 \
  --source-revision="$(git rev-parse HEAD)" --scenario=steady
GOODIX_TEST_SETUP_TRACE=/tmp/goodix-recording/setup.ini \
  meson test -C build goodixtls-driver --print-errorlogs
```

The importer also accepts `.pcap`/`.pcapng` directly when `--tsv` is omitted. It
accepts the `10052` and `10062` firmware profiles supported by the driver. The
import-to-driver regression exercises initialization, fresh TLS/image transfer
and immediate cancellation/retry with synthetic replies for both versions. It
selects OUT submissions and IN completions, validates lengths/checksums and the
complete eight-command setup sequence, and rejects missing/failed transfers. It
stops before the first TLS exchange, excluding encrypted traffic and images from
the fixture. The metadata contains firmware, declared expected USB ID, bus/address,
scenario, source revision and input SHA-256. Keep dirty-build identity and the
lifecycle log beside it; a revision alone does not identify uncommitted changes.
Imported provenance does not certify that a recording came from physical hardware.

The tshark fields are documented in the [USB field reference](https://www.wireshark.org/docs/dfref/u/usb.html)
and [tshark manual](https://www.wireshark.org/docs/man-pages/tshark.html).
The traditional [umockdev workflow](../tests/README.md) remains available for other
drivers. Recordings from real readers and hardware acceptance runs are still
needed; the software tests alone do not establish physical reliability.

## Offline recognition evaluation

`tools/goodix-score` extracts NBIS features once per assembled P5 (binary PGM),
8-bit, 500-DPI image and scores ordered pairs. It operates entirely on local files
and never installs prints or changes thresholds. It accepts 2–256 images and rejects
malformed dimensions/truncated files. Use final assembled images, not raw sensor bytes.

Collect assembled images with labels, without enrolling or deleting any prints:

```sh
sudo python tools/collect-goodix.py /private/captures/day1-index \
  --smoke build/tests/goodixtls-hardware-smoke --finger-id=person1-right-index \
  --session=day1 --condition=normal --cycles=3
```

Stop fprintd before collection and restore it afterward. The command requests six
swipes, saves normalized images with mode 0600 in a new mode-0700 directory, and
creates `manifest.csv`, `session.json` (build identity, file hashes, completion
status), and `lifecycle.jsonl`. It refuses to overwrite a session or store biometrics
inside the checkout. A failed run retains its successful captures with an explicit
failure status. When running as root the files are root-owned; evaluation needs
access to them as well. The collector also removes retired swipe/reset/image-dump
environment options for compatibility when testing older binaries. Installed-package users can call
`sudo goodix-collect` with the same labels and omit `--smoke`.

Repeat for other fingers, days and conditions, using a new directory for each run.
You can also create a CSV next to pre-existing captures:

```csv
path,finger_id,session,condition
a-day1.pgm,person1-right-index,day1,normal
a-day2.pgm,person1-right-index,day2,dry
b-day1.pgm,person2-right-index,day1,normal
b-day2.pgm,person2-right-index,day2,partial
```

Use stable pseudonyms for fingers and distinct session labels for separate recording
sessions. Include slow/partial swipes and dry/wet conditions across different days.
Obtain consent for any other person's captures; keep raw biometric files out of Git.

```sh
python tools/evaluate-recognition.py /private/captures/day1-index/manifest.csv \
  /private/captures/day2-index/manifest.csv /private/captures/day1-other/manifest.csv \
  --scorer build/tools/goodix-score --threshold=40 > /private/captures/evaluation.json
```

Each manifest resolves paths relative to its own directory. Duplicate paths and
identical image contents are rejected to prevent copied samples from inflating
accuracy. The report retains labels and hashes so every score can be traced to a
sample. Installed-package users can use `goodix-evaluate` without `--scorer`.
Failed capture attempts are recorded in each session's metadata/lifecycle log;
they have no image and do not enter the recognition-comparison denominator.

The report separates feature-extraction failures, genuine comparisons and impostor
comparisons, including counts, score ranges, error rates and results by probe condition.
Same-finger comparisons from the same session are excluded. Missing trial classes
report null rates. Comparisons share images and are not independent statistical trials.
The single-template evaluator is useful for comparing algorithms; it does not model
all six enrollment stages, PAM retries, or prove a production false-acceptance rate.
No threshold should be loosened from the results of a small sample. A representative
labeled corpus is still needed to assess actual recognition quality.

### Reference results

One person, one session (2026-10-07), firmware `GF3206_RTSEC_APP_10052`: 28
swipes of five fingers (6 each of right index, right middle, right ring and left
index; 4 of right thumb). The captures were made with the old driver, whose
images keep every stripe unchanged, so the same swipes were also run through
the current normalisation and stitching offline. "Best of the others" is what
fprintd decides on: a swipe's best score against the other samples of the
enrolled finger.

| | Old pipeline | Current pipeline |
| --- | --- | --- |
| Image length | 1008–2016 rows | 239–518 rows |
| Minutiae per image | 59–200 | 14–85 |
| Right index, best of the other 5 | 40–104 | 66–143 |
| Other fingers against right index, best of 6 | up to 45 (ring: 35–45) | up to 18 |
| Other fingers, any finger enrolled, best of gallery | up to 50 | up to 33 |
| Wrong-finger pairs reaching 24 | 152 of 624 | 2 of 624 |
| Wrong-finger pairs reaching 40 | 12 of 624 | 0 of 624 |

With the old pipeline at its threshold of 24 every right-ring swipe was
accepted as the right index. The current threshold of 40 sits above every
wrong-finger score above and well below the enrolled finger's. A second
session, other people's fingers and swipes from the fixed driver are still
needed.

