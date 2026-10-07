# pam_fprint_parallel: password or fingerprint at one prompt

The stock `pam_fprintd` asks for the finger first and the password only after
it gives up. `pam_fprint_parallel.so` shows one prompt and accepts whichever
comes first, with no fingerprint timeout:

```text
$ sudo true
[sudo] password or fingerprint for alice:
```

It talks to `fprintd` over D-Bus and does not depend on this fork's libfprint,
so it works with any reader fprintd supports. The source is
[pam/pam_fprint_parallel.c](../pam/pam_fprint_parallel.c); the Arch package
installs it as `/opt/libfprint-goodix/lib/security/pam_fprint_parallel.so`.

## Configuration

`goodix-fingerprint-pam install` (see [INSTALL_55a2.md](../INSTALL_55a2.md))
writes these lines, before each service's `auth include system-auth`:

```text
# /etc/pam.d/sudo
auth   requisite  pam_faillock.so preauth
-auth  [success=ok ignore=1 conv_err=die default=1]  /opt/libfprint-goodix/lib/security/pam_fprint_parallel.so mode=tty
auth   [default=done]  pam_faillock.so authsucc
auth   include  system-auth

# /etc/pam.d/polkit-1 (pkexec, run0, graphical administrator dialogs): the same with mode=conv
```

Read the control field as: a fingerprint match continues to the next line,
which resets `pam_faillock`'s failure counter and ends the stack; a typed
password (`PAM_IGNORE`), a missing or broken module, or any other error skips
that line and continues to the normal password stack; only a cancelled prompt
aborts. Do not use `default=die`: with it, removing the module would make the
whole service fail instead of falling back to the password.

| Option | Meaning |
| --- | --- |
| `mode=tty` | Read the password from the controlling terminal (for `sudo`). |
| `mode=conv` | Ask through the PAM conversation (for polkit's agent helper only; see below). |
| `timeout=N` | Seconds the fingerprint stays armed. Default `0`: until the prompt is answered. |
| `max-tries=N` | Non-matching fingers before only the password is accepted. Default `3`. |
| `lock-session=auto\|none\|ID` | Session whose lock releases the reader. Default `auto`: the caller's session, else the user's graphical session. |
| `debug` | Log decisions to syslog. |

`bus=ADDRESS` exists for the test suite. The module never reads the bus
address, or anything else, from the environment.

## Behaviour

| Event | Result |
| --- | --- |
| fprintd reports `verify-match` | `PAM_SUCCESS`; faillock's counter is reset and the stack ends |
| Password typed and Enter | Stored as `PAM_AUTHTOK`, `PAM_IGNORE`; `pam_unix` in `system-auth` checks it |
| Enter on an empty line | Ignored while the reader may still be used |
| Finger not recognized | "try again"; after `max-tries`, password only |
| Ctrl+C | Terminal restored, signal re-raised to the application |
| Dialog cancelled | `PAM_CONV_ERR`; the stack fails (`conv_err=die`) |
| Module missing | Skipped; the password prompt follows |

The module never checks a password itself. Wrong passwords are counted by
`pam_faillock` as usual, and the `faillock preauth` line in front of it keeps a
locked account locked for fingerprints too.

It steps aside, returning `PAM_IGNORE` without a prompt so the next module asks
for the password, when:

- the session is remote (`PAM_RHOST`, `SSH_*` variables, or a remote logind session);
- in `mode=tty`, standard input is not a terminal (`sudo -S`, `cat file | sudo tee …`);
- fprintd is not available, there is no reader, or the user has no enrolled finger.

## Terminal mode

The prompt is written to and read from `/dev/tty` with echo off. Backspace
deletes a character; Ctrl+U or Ctrl+W clears the line. Status messages ("Swipe was too short, try
again") appear on their own line, followed by the prompt again. The terminal
settings and signal handlers are restored on every exit path; Ctrl+C, Ctrl+\
and hangups are re-raised once they are, so `sudo` handles them as usual.

## Dialog mode

polkit's `polkit-agent-helper-1` runs PAM and relays the conversation to the
desktop's authentication agent. The module asks for the password in a helper
thread and watches fprintd meanwhile. KDE's dialog ignores the prompt text, so
the module also sends "Swipe your finger or type your password" and its status
messages as information messages, which the dialog shows above the password field.

After a fingerprint match the helper thread is still waiting for the password.
`polkit-agent-helper-1` reports success, the agent closes the connection, the
read ends and the process exits. The module is linked with `-z nodelete` so that
thread never runs in unloaded code. Status messages are sent from the main
thread while that prompt is pending. Use `mode=conv` only with applications that
exit after authenticating and whose conversation function accepts a message
from another thread during a prompt, like the polkit helper.

## Sharing the reader

fprintd lets one client use the reader at a time. While a prompt waits:

- the module releases the reader when the session locks, so the lock screen can
  use it, and claims it again after unlocking;
- a prompt that finds the reader busy (another prompt, the lock screen) keeps
  accepting the password and retries the reader every two seconds.

## Security notes

- Only a `verify-match` signal sent by the bus name that currently owns
  `net.reactivated.Fprint` counts. Any client on the system bus can address a
  signal to the module's connection; the module resolves fprintd's unique name
  (and logind's) and ignores everything else. Earlier development versions did
  not, and accepted forged signals: update if you installed one before this check.
- The module connects to the fixed system bus socket.
- A fingerprint prompt in a terminal cannot show *which* process is asking. A
  `sudo` waiting unseen in another terminal can receive a swipe meant for
  something else ([CVE-2024-37408](https://seclists.org/oss-sec/2024/q2/286)).
  Leaving the fingerprint out of `sudo` (`goodix-fingerprint-pam uninstall sudo`)
  and using `pkexec`/`run0`, whose dialog names the program, avoids this.
- A typed password is wiped from the module's buffers after it is handed to PAM.

## Logs

A successful fingerprint is logged as `fingerprint accepted for USER` (auth
facility). With `debug`, the module also logs why it stepped aside and each
fprintd status.

## Tests

[tests/test-pam-parallel.py](../tests/test-pam-parallel.py) loads the module
through libpam (`pam_start_confdir`) against a scripted fprintd and logind on a
private D-Bus. It types into a pseudo-terminal and covers matches, passwords,
retries, timeouts, Ctrl+C, a busy reader, lock and unlock, the step-aside
cases, dialog mode, and forged signals from another client. Run it with
`meson test -C build pam-parallel`.
