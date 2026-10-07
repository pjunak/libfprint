#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""pam_fprint_parallel against a scripted fprintd/logind on a private D-Bus.

Terminal-mode tests run the PAM application in a pseudo-terminal and type
into it; conversation-mode tests feed it through a pipe. A pam_exec step after
the module records the token pam_unix would receive ("asked" if it had to
prompt), so each test sees exactly what the rest of the stack gets.
"""
import errno
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parent
MODULE = os.environ.get('PAM_PARALLEL_MODULE')
HARNESS = os.environ.get('PAM_HARNESS')
USER = 'tester'
DBUS_CONFIG = '''<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>custom</type>
  <listen>unix:path={socket}</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_type="method_call"/>
    <allow send_type="signal"/>
    <allow send_type="method_return"/>
    <allow send_type="error"/>
    <allow receive_type="method_call"/>
    <allow receive_type="signal"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
  </policy>
</busconfig>
'''


def prerequisites():
    if not MODULE or not HARNESS:
        return 'PAM_PARALLEL_MODULE and PAM_HARNESS are not set'
    if not shutil.which('dbus-daemon'):
        return 'dbus-daemon is not installed'
    try:
        import gi
        gi.require_version('Gio', '2.0')
    except (ImportError, ValueError):
        return 'PyGObject is not installed'
    return None


class Parallel(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)
        self.socket = self.dir / 'bus'
        (self.dir / 'bus.conf').write_text(DBUS_CONFIG.format(socket=self.socket))
        self.bus = subprocess.Popen(['dbus-daemon', '--nofork', '--print-address=1',
                                     f'--config-file={self.dir / "bus.conf"}'],
                                    stdout=subprocess.PIPE, text=True)
        self.address = self.bus.stdout.readline().strip()
        self.fake = None
        self.log = self.dir / 'calls.log'
        self.token = self.dir / 'token'
        recorder = self.dir / 'record.sh'
        recorder.write_text(f'#!/bin/sh\ncat > {self.token}\n')
        recorder.chmod(0o755)
        # Stands in for `pam_faillock.so authsucc` in the production stack.
        self.reset = self.dir / 'faillock-reset'
        (self.dir / 'authsucc.sh').write_text(f'#!/bin/sh\ntouch {self.reset}\n')
        (self.dir / 'authsucc.sh').chmod(0o755)
        self.confdir = self.dir / 'pam.d'
        self.confdir.mkdir()

    def tearDown(self):
        for process in (self.fake, self.bus):
            if process:
                process.terminate()
                process.wait()
        self.temp.cleanup()

    def start_fprintd(self, **scenario):
        self.fake = subprocess.Popen([sys.executable, str(ROOT / 'fake-fprintd.py'), self.address,
                                      json.dumps(scenario), str(self.log)], stdout=subprocess.PIPE, text=True)
        self.assertEqual(self.fake.stdout.readline().strip(), 'READY')

    def configure(self, *options, module=None):
        """The production stack from goodix-fingerprint-pam, with stand-ins."""
        args = ' '.join([f'bus=unix:path={self.socket}', 'lock-session=none', *options])
        (self.confdir / 'test').write_text(
            f'-auth [success=ok ignore=1 conv_err=die default=1] {module or MODULE} {args}\n'
            f'auth [default=done] pam_exec.so {self.dir / "authsucc.sh"}\n'
            f'auth optional pam_exec.so expose_authtok {self.dir / "record.sh"}\n'
            'auth required pam_permit.so\n')

    def forge_later(self, delay):
        """From an unrelated client, send a forged verify-match and a forged
        lock to every connection (unicast) and to everyone (broadcast)."""
        def forge():
            import gi
            gi.require_version('Gio', '2.0')
            from gi.repository import Gio, GLib
            time.sleep(delay)
            flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
            bus = Gio.DBusConnection.new_for_address_sync(self.address, flags, None, None)
            names = bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                                  'ListNames', None, None, 0, -1, None).unpack()[0]
            for destination in [name for name in names if name.startswith(':')] + [None]:
                bus.emit_signal(destination, '/net/reactivated/Fprint/Device/0', 'net.reactivated.Fprint.Device',
                                'VerifyStatus', GLib.Variant('(sb)', ('verify-match', True)))
                bus.emit_signal(destination, '/org/freedesktop/login1/session/test',
                                'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                                GLib.Variant('(sa{sv}as)', ('org.freedesktop.login1.Session',
                                                            {'LockedHint': GLib.Variant('b', True)}, [])))
            bus.flush_sync(None)
            self.forged = True
        self.forged = False
        thread = threading.Thread(target=forge, daemon=True)
        thread.start()
        return thread

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def token_seen(self):
        return self.token.read_text().rstrip('\0') if self.token.exists() else None

    def environment(self):
        return {key: value for key, value in os.environ.items() if not key.startswith('SSH_')}

    def run_tty(self, keys=(), extra=(), deadline=15, expect_signal=None):
        """Run in a pseudo-terminal. keys: (seconds after the prompt, bytes)."""
        pid, master = pty.fork()
        if pid == 0:
            os.execve(HARNESS, [HARNESS, str(self.confdir), 'test', USER, *extra], self.environment())
        output, start, pending = b'', None, list(keys)
        end = time.monotonic() + deadline
        status = None
        while time.monotonic() < end:
            if start is None and b'password or fingerprint' in output:
                start = time.monotonic()
            while pending and start is not None and time.monotonic() - start >= pending[0][0]:
                os.write(master, pending.pop(0)[1])
            ready, _, _ = select.select([master], [], [], 0.05)
            if ready:
                try:
                    data = os.read(master, 4096)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    data = b''
                if not data:
                    break
                output += data
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                pid = None
                break
        else:
            os.kill(pid, signal.SIGKILL)
            self.fail(f'PAM did not finish: {output!r}')
        if pid:
            status = os.waitpid(pid, 0)[1]
        try:
            while select.select([master], [], [], 0.1)[0]:
                data = os.read(master, 4096)
                if not data:
                    break
                output += data
        except OSError:
            pass
        attributes = termios.tcgetattr(master)
        os.close(master)
        output = output.decode(errors='replace')
        if expect_signal is None:
            self.assertTrue(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0, f'{status}: {output}')
        return output, status, attributes

    def run_pipe(self, *extra, stdin=None, deadline=15):
        process = subprocess.run([HARNESS, str(self.confdir), 'test', USER, *extra], input=stdin,
                                 capture_output=True, text=True, timeout=deadline, env=self.environment())
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        return process.stdout

    # ---- terminal mode ----

    def test_fingerprint_match_restores_terminal(self):
        self.start_fprintd(verify=['match'])
        self.configure('mode=tty')
        output, status, attributes = self.run_tty()
        self.assertIn('[test] password or fingerprint for tester:', output)
        self.assertIn('RESULT:Success', output)
        self.assertIn('TERMIOS:echo=1,icanon=1', output)
        self.assertIsNone(self.token_seen())  # the password stack never ran
        self.assertTrue(self.reset.exists())  # faillock's counter is reset
        self.assertEqual(self.calls()[-4:], ['VerifyStart any', 'emit verify-match', 'VerifyStop', 'Release'])

    def test_typed_password_goes_to_password_stack(self):
        self.start_fprintd(verify=['wait'])
        self.configure('mode=tty')
        # Habitual empty Enter is ignored while the finger may arrive; editing works.
        output, status, attributes = self.run_tty([(0.3, b'\r'), (0.5, b'hunX\x7fter2\r')])
        self.assertIn('RESULT:Success', output)
        self.assertEqual(self.token_seen(), 'hunter2')
        self.assertNotIn('hunter2', output)  # never echoed
        self.assertFalse(self.reset.exists())  # only pam_unix decides about passwords
        self.assertEqual(self.calls()[-2:], ['VerifyStop', 'Release'])

    def test_failed_fingers_leave_the_password(self):
        self.start_fprintd(verify=['no-match', ['too-short', 'retry', 'no-match']], delay_ms=100)
        self.configure('mode=tty', 'max-tries=2')
        output, status, attributes = self.run_tty([(1.5, b'secret\r')])
        self.assertIn('Fingerprint not recognized, try again', output)
        self.assertIn('Swipe was too short, try again', output)
        self.assertIn('Swipe your finger again', output)  # the reader's scan-type was read
        self.assertIn('Fingerprint not recognized; type your password', output)
        self.assertEqual(self.token_seen(), 'secret')
        self.assertEqual(self.calls().count('VerifyStart any'), 2)
        self.assertEqual(self.calls()[-2:], ['VerifyStop', 'Release'])

    def test_ctrl_c_restores_terminal_and_reaches_application(self):
        self.start_fprintd(verify=['wait'])
        self.configure('mode=tty')
        output, status, attributes = self.run_tty([(0.3, b'abc\x03')], expect_signal=signal.SIGINT)
        self.assertTrue(os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGINT, output)
        self.assertTrue(attributes[3] & termios.ECHO and attributes[3] & termios.ICANON)
        self.assertEqual(self.calls()[-2:], ['VerifyStop', 'Release'])

    def test_no_timeout_by_default_but_optional(self):
        self.start_fprintd(verify=['wait'])
        self.configure('mode=tty', 'timeout=1')
        output, status, attributes = self.run_tty([(2.0, b'pw\r')])
        self.assertIn('Fingerprint timed out; type your password', output)
        self.assertEqual(self.token_seen(), 'pw')
        self.assertEqual(self.calls().count('Release'), 1)

    def test_busy_reader_is_retried(self):
        self.start_fprintd(busy_claims=1, verify=['match'])
        self.configure('mode=tty')
        output, status, attributes = self.run_tty()
        self.assertIn('RESULT:Success', output)
        self.assertEqual(self.calls().count('Claim tester'), 2)
        self.assertIsNone(self.token_seen())

    def test_lock_releases_and_unlock_rearms(self):
        self.start_fprintd(verify=['wait', 'match'], locks=[[400, True], [900, False]])
        self.configure('mode=tty', 'lock-session=test')
        output, status, attributes = self.run_tty()
        self.assertIn('RESULT:Success', output)
        calls = self.calls()
        locked, unlocked = calls.index('locked True'), calls.index('locked False')
        self.assertEqual(calls[locked + 1:locked + 3], ['VerifyStop', 'Release'])
        self.assertIn('Claim tester', calls[unlocked:])
        self.assertIsNone(self.token_seen())

    def test_forged_signals_are_ignored(self):
        # Any client can address a signal to the module's connection. Only
        # fprintd's (and logind's) unique bus name may decide anything.
        self.start_fprintd(verify=['wait'])
        self.configure('mode=tty', 'lock-session=test')
        self.forge_later(0.5)
        output, status, attributes = self.run_tty([(2.0, b'realpassword\r')])
        self.assertTrue(self.forged)
        self.assertEqual(self.token_seen(), 'realpassword')  # still waited for the user
        self.assertEqual(self.calls().count('Release'), 1)  # the forged lock released nothing

    def test_forged_signals_are_ignored_in_dialog(self):
        self.start_fprintd(verify=['wait'])
        self.configure('mode=conv')
        read_end, write_end = os.pipe()
        process = subprocess.Popen([HARNESS, str(self.confdir), 'test', USER], stdin=read_end,
                                   stdout=subprocess.PIPE, text=True, env=self.environment())
        os.close(read_end)
        try:
            self.forge_later(0.5).join(5)
            time.sleep(0.5)
            os.write(write_end, b'dialogpassword\n')
            output = process.communicate(timeout=10)[0]
        finally:
            os.close(write_end)
            process.kill()
        self.assertTrue(self.forged)
        self.assertIn('RESULT:Success', output)
        self.assertEqual(self.token_seen(), 'dialogpassword')

    # ---- falling back to the usual prompt ----

    def assert_fell_back(self, output):
        self.assertIn('RESULT:Success', output)
        self.assertNotIn('password or fingerprint', output)
        self.assertEqual(self.token_seen(), 'asked')  # pam_exec had to prompt
        self.assertNotIn('Claim tester', self.calls())

    def test_missing_module_falls_back_to_password(self):
        # Removing the package without updating PAM must not lock anyone out.
        self.start_fprintd(verify=['match'])
        self.configure('mode=conv', module='/nonexistent/pam_fprint_parallel.so')
        output = self.run_pipe(stdin='')
        self.assertIn('RESULT:Success', output)
        self.assertEqual(self.token_seen(), 'asked')
        self.assertFalse(self.reset.exists())

    def test_no_enrolled_finger(self):
        self.start_fprintd(enrolled=[])
        self.configure('mode=tty')
        self.assert_fell_back(self.run_tty()[0])

    def test_no_fprintd(self):
        self.configure('mode=tty')
        self.assert_fell_back(self.run_tty()[0])

    def test_remote_session(self):
        self.start_fprintd(verify=['match'])
        self.configure('mode=tty')
        self.assert_fell_back(self.run_tty(extra=['--rhost', 'example.org'])[0])

    def test_piped_stdin(self):
        self.start_fprintd(verify=['match'])
        self.configure('mode=tty')
        self.assert_fell_back(self.run_pipe(stdin=''))

    # ---- conversation mode (polkit) ----

    def test_conversation_password(self):
        self.start_fprintd(verify=['wait'])
        self.configure('mode=conv')
        output = self.run_pipe('--answer', 'fromdialog')
        # KDE's dialog ignores the prompt text and shows info messages.
        self.assertLess(output.index('INFO:Swipe your finger or type your password'),
                        output.index('PROMPT:Password or fingerprint:'))
        self.assertEqual(self.token_seen(), 'fromdialog')
        self.assertEqual(self.calls()[-2:], ['VerifyStop', 'Release'])

    def test_conversation_fingerprint_while_dialog_waits(self):
        self.start_fprintd(verify=['match'])
        self.configure('mode=conv')
        # The conversation blocks on an open, silent stdin like polkit's agent
        # helper. The result arrives while it waits; the helper process exits
        # once the agent, having read SUCCESS, closes the connection.
        read_end, write_end = os.pipe()
        process = subprocess.Popen([HARNESS, str(self.confdir), 'test', USER, '--linger-ms', '1000'],
                                   stdin=read_end, stdout=subprocess.PIPE, text=True, env=self.environment())
        os.close(read_end)
        output, end = '', time.monotonic() + 10
        try:
            while 'RESULT:' not in output and time.monotonic() < end:
                if select.select([process.stdout], [], [], 0.1)[0]:
                    output += os.read(process.stdout.fileno(), 4096).decode()
            self.assertIn('RESULT:Success', output)
            os.close(write_end)
            write_end = None
            output += process.communicate(timeout=5)[0]
            # The leftover conversation thread finishes inside the module after
            # pam_end(); the process must still exit cleanly.
            self.assertEqual(process.returncode, 0, output)
        finally:
            if write_end is not None:
                os.close(write_end)
            process.kill()
        self.assertIsNone(self.token_seen())
        self.assertEqual(self.calls()[-2:], ['VerifyStop', 'Release'])

    def test_conversation_feedback_while_dialog_waits(self):
        self.start_fprintd(verify=[['too-short', 'no-match'], 'match'], delay_ms=150)
        self.configure('mode=conv')
        read_end, write_end = os.pipe()
        process = subprocess.Popen([HARNESS, str(self.confdir), 'test', USER, '--linger-ms', '300'],
                                   stdin=read_end, stdout=subprocess.PIPE, text=True, env=self.environment())
        os.close(read_end)
        output, end = '', time.monotonic() + 10
        try:
            while 'RESULT:' not in output and time.monotonic() < end:
                if select.select([process.stdout], [], [], 0.1)[0]:
                    output += os.read(process.stdout.fileno(), 4096).decode()
            os.close(write_end)
            write_end = None
            output += process.communicate(timeout=5)[0]
            self.assertEqual(process.returncode, 0, output)
        finally:
            if write_end is not None:
                os.close(write_end)
            process.kill()
        # Messages reach the dialog while its password request is pending.
        prompt = output.index('PROMPT:Password or fingerprint:')
        self.assertGreater(output.index('INFO:Swipe was too short, try again'), prompt)
        self.assertGreater(output.index('INFO:Fingerprint not recognized, try again'), prompt)
        self.assertIn('RESULT:Success', output)

    def test_conversation_cancelled(self):
        self.start_fprintd(verify=['wait'])
        self.configure('mode=conv')
        # Closing the dialog ends the conversation; the stack fails at once.
        output = self.run_pipe(stdin='')
        self.assertIn('RESULT:Conversation error', output)
        self.assertFalse(self.reset.exists())
        self.assertIsNone(self.token_seen())
        self.assertEqual(self.calls()[-2:], ['VerifyStop', 'Release'])


if __name__ == '__main__':
    missing = prerequisites()
    if missing:
        print(f'SKIP: {missing}')
        sys.exit(77)
    unittest.main()
