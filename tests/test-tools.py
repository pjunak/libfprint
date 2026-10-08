#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression tests for PAM resolution and unbiased evaluation accounting."""
import importlib.util
import configparser
import csv
import io
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / file)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


diag = module('diagnose', 'goodix-diagnose.py')
evaluation = module('evaluation', 'evaluate-recognition.py')
trace = module('trace_import', 'import-goodix-trace.py')
collector = module('collector', 'collect-goodix.py')
fingerprint_pam = module('fingerprint_pam', 'goodix-fingerprint-pam.py')

# Vendor policies on Arch/CachyOS (plasma-login-manager 6.7.4, polkit 127, sudo 1.9.17).
VENDOR_PLASMALOGIN = '''#%PAM-1.0

# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: none

auth        include     system-login
-auth       optional    pam_gnome_keyring.so
-auth       optional    pam_kwallet5.so

account     include     system-login

password    include     system-login
-password   optional    pam_gnome_keyring.so    use_authtok

session     optional    pam_keyinit.so          force revoke
session     include     system-login
-session    optional    pam_gnome_keyring.so    auto_start
-session    optional    pam_kwallet5.so         auto_start
'''
VENDOR_POLKIT = '''#%PAM-1.0

auth       include      system-auth
account    include      system-auth
password   include      system-auth
session    include      system-auth
'''
DISTRO_SUDO = '''#%PAM-1.0
auth\t\tinclude\t\tsystem-auth
account\t\tinclude\t\tsystem-auth
session\t\tinclude\t\tsystem-auth
session\t\toptional\tpam_systemd.so class=none
'''


class FingerprintPam(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for directory in ('etc/pam.d', 'usr/lib/pam.d', 'etc/systemd/system/fprintd.service.d',
                          'opt/libfprint-goodix/lib'):
            (self.root / directory).mkdir(parents=True)
        (self.root / 'usr/lib/pam.d/plasmalogin').write_text(VENDOR_PLASMALOGIN)
        (self.root / 'usr/lib/pam.d/polkit-1').write_text(VENDOR_POLKIT)
        (self.root / 'etc/pam.d/sudo').write_text(DISTRO_SUDO)
        (self.root / 'etc/systemd/system/fprintd.service.d/99-goodix-local.conf').write_text(
            '[Service]\nEnvironment=LD_LIBRARY_PATH=/opt/libfprint-goodix/lib\n')
        (self.root / 'opt/libfprint-goodix/lib/libfprint-2.so.2').write_text('')
        self.module = self.root / fingerprint_pam.PARALLEL.lstrip('/')
        self.module.parent.mkdir(parents=True)
        self.module.write_text('')

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        from contextlib import redirect_stdout, redirect_stderr
        with redirect_stdout(out), redirect_stderr(err):
            code = fingerprint_pam.main(['--root', str(self.root), *args])
        return code, out.getvalue() + err.getvalue()

    def auth_lines(self, name):
        text = (self.root / 'etc/pam.d' / name).read_text()
        # Type, control (possibly a bracketed list) and module, as PAM parses them.
        return [list(match.groups()) for match in re.finditer(r'^(-?auth)\s+(\[[^]]*\]|\S+)\s+(\S+)', text, re.M)]

    def test_example_matches_generated_login_policy(self):
        # The shipped example is exactly what install writes for this vendor policy.
        example = (ROOT / 'data/pam/plasmalogin-fingerprint.example').read_text()
        self.assertEqual(fingerprint_pam.render('plasmalogin', VENDOR_PLASMALOGIN), example)

    def test_install_orders_checks_password_and_fingerprint(self):
        self.assertEqual(self.run_tool('install', 'plasmalogin', 'sudo', 'polkit-1')[0], 0)
        login = self.auth_lines('plasmalogin')
        modules = [line[2] for line in login]
        # Account checks run before the fingerprint can succeed; a typed
        # password is tried first and skips exactly the fingerprint line.
        self.assertEqual(modules[:6], ['pam_shells.so', 'pam_nologin.so', 'pam_faillock.so',
                                       'pam_unix.so', 'pam_fprintd.so', 'pam_faillock.so'])
        # A correct password jumps over the fingerprint and the counter reset.
        self.assertEqual(login[3][1], '[success=2 new_authtok_reqd=2 default=ignore]')
        self.assertEqual(login[4][:2], ['-auth', '[success=ok default=1]'])
        self.assertEqual(login[5][1], '[default=done]')
        self.assertEqual(login[6][1:], ['include', 'system-login'])
        # sudo and polkit: one prompt for password or finger; only a match
        # ends the stack, a typed password continues to pam_unix.
        for name, mode in (('sudo', 'mode=tty'), ('polkit-1', 'mode=conv')):
            lines = self.auth_lines(name)
            self.assertEqual([line[2] for line in lines[:4]],
                             ['pam_faillock.so', fingerprint_pam.PARALLEL, 'pam_faillock.so', 'system-auth'])
            # A match resets faillock and ends the stack; anything else, a
            # missing module included, falls through to the password.
            self.assertEqual(lines[1][:2], ['-auth', '[success=ok ignore=1 conv_err=die default=1]'])
            self.assertEqual(lines[2][1], '[default=done]')
            self.assertIn('authsucc', (self.root / 'etc/pam.d' / name).read_text())
            self.assertIn(mode, (self.root / 'etc/pam.d' / name).read_text())
        self.assertEqual(self.run_tool('check'), (0, ''))
        self.assertIn('already enabled', self.run_tool('install')[1])

    def test_uninstall_restores_distribution_policy(self):
        (self.root / 'etc/pam.d/plasmalogin').write_text('# earlier hand-made override\n' + VENDOR_PLASMALOGIN)
        self.assertEqual(self.run_tool('install', 'plasmalogin', 'sudo', 'polkit-1')[0], 0)
        backups = list((self.root / fingerprint_pam.STATE_DIR / 'backup').iterdir())
        self.assertEqual(sorted(path.name.split('.')[0] for path in backups), ['plasmalogin', 'sudo'])
        self.assertEqual(self.run_tool('uninstall')[0], 0)
        self.assertFalse((self.root / 'etc/pam.d/plasmalogin').exists())
        self.assertFalse((self.root / 'etc/pam.d/polkit-1').exists())
        self.assertEqual((self.root / 'etc/pam.d/sudo').read_text(), DISTRO_SUDO)
        self.assertIn('not managed', self.run_tool('uninstall', 'sudo')[1])

    def test_check_reports_vendor_changes_pacnew_and_library(self):
        self.run_tool('install')
        self.assertEqual(self.run_tool('check')[0], 0)
        (self.root / 'usr/lib/pam.d/polkit-1').write_text(VENDOR_POLKIT + 'auth optional pam_permit.so\n')
        (self.root / 'etc/pam.d/sudo.pacnew').write_text(DISTRO_SUDO)
        code, output = self.run_tool('check')
        self.assertEqual(code, 1)
        self.assertIn('polkit-1: the distribution changed', output)
        self.assertIn('goodix-fingerprint-pam install polkit-1"', output)
        self.assertIn('sudo.pacnew', output)
        self.assertIn('OUT OF DATE', self.run_tool('status')[1])
        # Regenerating follows the new vendor policy and keeps one block.
        self.run_tool('install', 'polkit-1')
        (self.root / 'etc/pam.d/sudo.pacnew').unlink()
        self.assertEqual(self.run_tool('check')[0], 0)
        self.assertEqual((self.root / 'etc/pam.d/polkit-1').read_text().count(fingerprint_pam.BEGIN), 1)
        # An older install's drop-in sorts after the package's and wins.
        old_library = self.root / 'opt/fprint55a2/lib'
        old_library.mkdir(parents=True)
        (old_library / 'libfprint-2.so.2').write_text('')
        override = self.root / 'etc/systemd/system/fprintd.service.d/override.conf'
        override.write_text('[Service]\nEnvironment=LD_LIBRARY_PATH=/opt/fprint55a2/lib\n')
        code, output = self.run_tool('check')
        self.assertEqual(code, 1)
        self.assertIn('fprintd uses /opt/fprint55a2/lib (set in', output)
        self.assertIn('override.conf', output)
        override.unlink()
        self.assertEqual(self.run_tool('check')[0], 0)
        (self.root / 'etc/systemd/system/fprintd.service.d/99-goodix-local.conf').unlink()
        code, output = self.run_tool('check')
        self.assertEqual(code, 1)
        self.assertIn('no 55a2 driver', output)

    def test_login_screen_is_opt_in(self):
        # A fingerprint login leaves KWallet and GNOME Keyring locked, so the
        # default covers sudo and polkit only and never touches the greeter.
        self.assertEqual(self.run_tool('install')[0], 0)
        self.assertFalse((self.root / 'etc/pam.d/plasmalogin').exists())
        self.assertIn('pam_fprint_parallel.so', (self.root / 'etc/pam.d/sudo').read_text())
        self.assertNotIn('plasmalogin', self.run_tool('diff')[1])
        self.assertIn('password only', self.run_tool('status')[1].splitlines()[0])
        # Explicitly enabled and later removed again, the vendor policy returns.
        self.run_tool('install', 'plasmalogin')
        self.assertTrue((self.root / 'etc/pam.d/plasmalogin').exists())
        self.assertEqual(self.run_tool('uninstall', 'plasmalogin')[0], 0)
        self.assertFalse((self.root / 'etc/pam.d/plasmalogin').exists())
        self.assertIn('pam_fprint_parallel.so', (self.root / 'etc/pam.d/sudo').read_text())
        self.assertIn('sudo             fingerprint, managed', self.run_tool('status')[1])

    def test_check_reports_updated_setup_and_missing_module(self):
        # A block written by an older version (sequential pam_fprintd) is
        # reported as an update, not as a distribution change.
        old = fingerprint_pam.render('sudo', DISTRO_SUDO).replace(
            f'{fingerprint_pam.PARALLEL_CONTROL}  {fingerprint_pam.PARALLEL} mode=tty',
            'sufficient  pam_fprintd.so max-tries=2 timeout=10')
        (self.root / 'etc/pam.d/sudo').write_text(old)
        code, output = self.run_tool('check', 'sudo')
        self.assertEqual(code, 1)
        self.assertIn('sets it up differently', output)
        self.assertIn('install sudo"', output)
        self.run_tool('install', 'sudo')
        self.assertEqual(self.run_tool('check', 'sudo')[0], 0)
        self.module.unlink()
        code, output = self.run_tool('check', 'sudo')
        self.assertEqual(code, 1)
        self.assertIn('pam_fprint_parallel.so is missing', output)
        self.assertIn('uninstall sudo"', output)

    def test_unexpected_policy_changes_nothing(self):
        (self.root / 'usr/lib/pam.d/polkit-1').write_text('#%PAM-1.0\nauth required pam_unix.so\n')
        code, output = self.run_tool('install')
        self.assertEqual(code, 1)
        self.assertIn('needs a manual review', output)
        self.assertFalse((self.root / 'etc/pam.d/plasmalogin').exists())
        self.assertEqual((self.root / 'etc/pam.d/sudo').read_text(), DISTRO_SUDO)


class Tools(unittest.TestCase):
    def test_usb_id_does_not_identify_firmware(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            device = root / 'sys/bus/usb/devices/3-3'
            device.mkdir(parents=True)
            (device / 'idVendor').write_text('27c6\n')
            (device / 'idProduct').write_text('55a2\n')
            report = diag.collect(root)['devices'][0]
            self.assertEqual(report['profile'], '55a2')
            self.assertEqual(report['supported_firmware'],
                             ['GF3206_RTSEC_APP_10052', 'GF3206_RTSEC_APP_10062'])
            self.assertNotIn('firmware', report)
            (device / 'idProduct').write_text('55b4\n')
            self.assertEqual(diag.collect(root)['devices'][0]['supported_firmware'], [])

    def test_latest_firmware_from_success_or_rejection(self):
        lines = ['Device firmware: "GF3206_RTSEC_APP_10062"',
                 "failed during activation: Unsupported Goodix firmware 'GF3206_RTSEC_APP_10052'; "
                 'this build supports 55a2 / GF3206_RTSEC_APP_10062 (code: 15)']
        self.assertEqual(diag.firmware_from_journal('\n'.join(lines)), 'GF3206_RTSEC_APP_10052')
        self.assertEqual(diag.firmware_from_journal('\n'.join(reversed(lines))), 'GF3206_RTSEC_APP_10062')
        self.assertIsNone(diag.firmware_from_journal('Starting Fingerprint Authentication Daemon...'))

    def test_pam_overrides_and_auth_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for directory in ('etc/pam.d', 'usr/lib/pam.d'):
                (root / directory).mkdir(parents=True)
            (root / 'usr/lib/pam.d/plasmalogin').write_text('auth required pam_fprintd.so\n')
            (root / 'etc/pam.d/plasmalogin').write_text('auth include common\nsession optional pam_fprintd.so\n')
            (root / 'etc/pam.d/common').write_text('auth [success=1 default=ignore] pam_unix.so\nauth include plasmalogin\n')
            report = diag.collect(root)
            self.assertFalse(report['fingerprint_module_present']['plasmalogin'])
            self.assertEqual(len(report['pam_auth']['plasmalogin']), 1)
            (root / 'etc/pam.d/common').write_text('auth sufficient /usr/lib/security/pam_fprintd.so\n')
            self.assertTrue(diag.collect(root)['fingerprint_module_present']['plasmalogin'])
            # The combined prompt counts too, with its bracketed control.
            (root / 'etc/pam.d/sudo').write_text(
                '-auth [success=ok ignore=1 conv_err=die default=1] '
                '/opt/libfprint-goodix/lib/security/pam_fprint_parallel.so mode=tty\n')
            self.assertTrue(diag.collect(root)['fingerprint_module_present']['sudo'])

    def test_evaluation_excludes_same_session_and_counts_failures(self):
        rows = [{'finger_id': 'a', 'session': '1', 'condition': 'dry'},
                {'finger_id': 'a', 'session': '1', 'condition': 'dry'},
                {'finger_id': 'a', 'session': '2', 'condition': 'normal'},
                {'finger_id': 'b', 'session': '1', 'condition': 'wet'}]
        result = {'images': [{'index': i, 'usable': i != 3} for i in range(4)],
                  'scores': [{'probe': 0, 'gallery': 1, 'score': 100},
                             {'probe': 1, 'gallery': 0, 'score': 100},
                             {'probe': 0, 'gallery': 2, 'score': 23},
                             {'probe': 2, 'gallery': 0, 'score': 25},
                             {'probe': 1, 'gallery': 2, 'score': 24},
                             {'probe': 2, 'gallery': 1, 'score': 24}]}
        report = evaluation.summarize(rows, result, 24)
        self.assertEqual(report['genuine']['trials'], 4)
        self.assertEqual(report['genuine']['errors'], 1)
        self.assertEqual(report['feature_extraction_failures'], 1)
        self.assertIsNone(report['impostor']['error_rate'])

    def test_threshold_equality_accepts(self):
        rows = [{'finger_id': 'a', 'session': '1', 'condition': 'normal'},
                {'finger_id': 'b', 'session': '1', 'condition': 'normal'}]
        result = {'images': [{'index': i, 'usable': True} for i in range(2)],
                  'scores': [{'probe': 0, 'gallery': 1, 'score': 24},
                             {'probe': 1, 'gallery': 0, 'score': 23}]}
        self.assertEqual(evaluation.summarize(rows, result, 24)['impostor']['errors'], 1)

    def test_reject_incomplete_scores(self):
        rows = [{'finger_id': 'a', 'session': '1', 'condition': 'normal'},
                {'finger_id': 'b', 'session': '1', 'condition': 'normal'}]
        result = {'images': [{'index': i, 'usable': True} for i in range(2)], 'scores': []}
        with self.assertRaisesRegex(ValueError, 'every usable comparison'):
            evaluation.summarize(rows, result, 24)

    def test_merge_manifests_and_reject_duplicate_contents(self):
        with tempfile.TemporaryDirectory() as temp:
            manifests = []
            for i in range(2):
                directory = Path(temp) / str(i)
                directory.mkdir()
                (directory / 'image.pgm').write_bytes(bytes([i]))
                manifest = directory / 'manifest.csv'
                manifest.write_text(f'path,finger_id,session,condition\nimage.pgm,finger,{i},normal\n')
                manifests.append(manifest)
            rows = evaluation.load_samples(manifests)
            self.assertEqual([row['session'] for row in rows], ['0', '1'])
            self.assertNotEqual(rows[0]['path'], rows[1]['path'])
            (manifests[1].parent / 'image.pgm').write_bytes(bytes([0]))
            with self.assertRaisesRegex(ValueError, 'Identical captures'):
                evaluation.load_samples(manifests)

    def test_collection_keeps_partial_run_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            helper = root / 'capture-helper'
            helper.write_text('''#!/usr/bin/env python3
import os, pathlib, sys
assert 'GOODIX_SAVE_DIR' not in os.environ
directory = pathlib.Path(next(x.split('=', 1)[1] for x in sys.argv if x.startswith('--output-dir=')))
(directory / 'capture-0001-1.pgm').write_bytes(b'P5\\n16 16\\n255\\n' + bytes(256))
print('{"passed":false}')
sys.exit(1)
''')
            helper.chmod(0o700)
            target = root / 'private-session'
            status, count = collector.collect(target, helper, 'finger-a', 'day1', 'dry', 1, 1000)
            self.assertEqual((status, count), (1, 1))
            self.assertEqual(target.stat().st_mode & 0o777, 0o700)
            metadata = json.loads((target / 'session.json').read_text())
            self.assertEqual(metadata['exit_status'], 1)
            self.assertEqual(metadata['captures'], 1)
            with (target / 'manifest.csv').open() as file:
                rows = list(csv.DictReader(file))
            self.assertEqual(rows[0]['session'], 'day1')
            with self.assertRaises(FileExistsError):
                collector.collect(target, helper, 'finger-a', 'day2', 'dry', 1, 1000)


class TraceImport(unittest.TestCase):
    def setUp(self):
        self.fixture = configparser.ConfigParser()
        self.fixture.read(ROOT / 'tests/goodix-setup-replay.ini')
        self.rows = []
        for i in range(8):
            for endpoint, key, event in ((1, 'request', '0x53'), (0x82, 'reply', '0x43')):
                data = bytes.fromhex(self.fixture[f'exchange-{i}'][key])
                # Real host requests arrive as separate 64-byte USB submissions.
                for offset in range(0, len(data), 64):
                    chunk = data[offset:offset + 64]
                    self.rows.append(['3', '7', hex(endpoint), event, '0', str(len(chunk)), chunk.hex(':')])
                    # Duplicate payload on the other URB event must not be replayed.
                    duplicate = self.rows[-1].copy()
                    duplicate[3] = '0x43' if event == '0x53' else '0x53'
                    self.rows.append(duplicate)
        body = b'\xd0\x03\x00\x00\x00\xd7'
        head = b'\xa0' + struct.pack('<H', len(body))
        self.tls_request = (head + bytes([sum(head) & 255]) + body).ljust(64, b'\0')
        self.rows.append(['3', '7', '0x01', '0x53', '0', '64', self.tls_request.hex()])

    def rows_with_firmware(self, firmware):
        # Replace only the synthetic firmware reply, retaining the setup/config
        # from the fixture. Regenerate lengths/checksums for a valid wire message.
        payload = firmware.encode('ascii')
        body = b'\xa8' + struct.pack('<H', len(payload) + 1) + payload
        body += bytes([(0xaa - sum(body)) & 255])
        head = b'\xa0' + struct.pack('<H', len(body))
        reply = (head + bytes([sum(head) & 255]) + body).ljust(64, b'\0')
        rows = [row.copy() for row in self.rows]
        for row in rows:
            data = bytes.fromhex(row[6].replace(':', ''))
            if row[2] == '0x82' and data[4] == 0xa8:
                row[5], row[6] = str(len(reply)), reply.hex()
        return rows

    def test_supported_firmware_and_unknown_variants(self):
        for firmware in ('GF3206_RTSEC_APP_10052', 'GF3206_RTSEC_APP_10062'):
            with self.subTest(firmware=firmware):
                _, observed = trace.import_rows(self.rows_with_firmware(firmware), 3, 7)
                self.assertEqual(observed, firmware)
        for firmware in ('GF3206_RTSEC_APP_10053', 'GF3206_RTSEC_APP_10052_extra',
                         'GF3268_RTSEC_APP_10041'):
            with self.subTest(firmware=firmware):
                with self.assertRaisesRegex(ValueError, 'Unsupported recorded firmware'):
                    trace.import_rows(self.rows_with_firmware(firmware), 3, 7)

    def test_setup_roundtrip_excludes_recorded_tls(self):
        self.rows.append(['3', '7', '0x82', '0x43', '0', '7', 'private TLS data'])
        # Other devices on the same bus do not enter the fixture.
        self.rows.insert(0, ['3', '8', '0x01', '0x53', '0', '6', 'not our data'])
        steps, firmware = trace.import_rows(self.rows, 3, 7)
        self.assertEqual(firmware, 'GF3206_RTSEC_APP_10062')
        for i, (request, reply) in enumerate(steps):
            self.assertEqual(request.hex(), self.fixture[f'exchange-{i}']['request'])
            self.assertEqual(reply.hex(), self.fixture[f'exchange-{i}']['reply'])
        output = io.StringIO()
        trace.write_fixture(output, steps, firmware, 'a' * 64, '072991a', 'warm-boot', 3, 7)
        self.assertIn('source-sha256=', output.getvalue())
        self.assertNotIn('private TLS data', output.getvalue())

    @unittest.skipUnless(os.getenv('GOODIX_TEST_DRIVER'), 'Goodix test driver is not built')
    def test_imported_fixture_runs_through_driver(self):
        for firmware in ('GF3206_RTSEC_APP_10052', 'GF3206_RTSEC_APP_10062'):
            with self.subTest(firmware=firmware):
                self.run_imported_fixture(self.rows_with_firmware(firmware))

    def run_imported_fixture(self, rows):
        with tempfile.TemporaryDirectory() as temp:
            source, output = Path(temp) / 'usbmon.tsv', Path(temp) / 'setup.ini'
            with source.open('w', newline='') as file:
                csv.writer(file, delimiter='\t').writerows(rows)
            subprocess.run([sys.executable, str(ROOT / 'tools/import-goodix-trace.py'),
                            str(source), str(output), '--tsv', '--bus=3', '--address=7',
                            '--source-revision=072991a'], check=True, timeout=10)
            result = subprocess.run([os.environ['GOODIX_TEST_DRIVER'], '-p',
                                     '/goodixtls/driver/setup-replay-fragmented'],
                                    env={**os.environ, 'GOODIX_TEST_SETUP_TRACE': str(output)},
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_truncation_usb_error_and_bad_checksum(self):
        for change in ('truncation', 'usb-error', 'checksum'):
            with self.subTest(change=change):
                rows = [row.copy() for row in self.rows]
                if change == 'truncation':
                    rows[0][5] = '65'
                elif change == 'usb-error':
                    rows[1][4] = '-32'
                else:
                    rows[0][6] = 'ff' + rows[0][6][2:]
                with self.assertRaises(ValueError):
                    trace.import_rows(rows, 3, 7)

    def test_rejects_incomplete_sequence(self):
        with self.assertRaisesRegex(ValueError, 'complete initialization'):
            trace.import_rows(self.rows[:-1], 3, 7)

    def test_rejects_missing_response(self):
        # Remove the actual IN completions, leaving the ignored duplicate submissions.
        rows = [row for row in self.rows if row[2] != '0x82' or row[3] != '0x43']
        with self.assertRaisesRegex(ValueError, 'ACK'):
            trace.import_rows(rows, 3, 7)

    def test_ack_may_report_missing_config(self):
        row = next(row for row in self.rows if row[2] == '0x82' and row[3] == '0x43')
        packet = bytearray.fromhex(row[6].replace(':', ''))
        packet[8] = 3  # Valid ACK with has_no_config set before config upload.
        packet[9] = (packet[9] - 2) & 255
        row[6] = packet.hex()
        steps, _ = trace.import_rows(self.rows, 3, 7)
        self.assertEqual(steps[1][1][8], 3)


if __name__ == '__main__':
    unittest.main()
