#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Collect labeled, private Goodix captures without changing enrolled prints."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def collect(directory, smoke, finger_id, session, condition, cycles, delay_ms):
    directory.mkdir(mode=0o700)  # Refuse existing directories, including symlinks.
    env = os.environ.copy()
    for name in ('GOODIX_SAVE_DIR', 'GOODIX_SWIPE_FDT', 'GOODIX_USB_RESET'):
        env.pop(name, None)
    env['FP_DRIVERS_WHITELIST'] = 'goodixtls55x4'
    command = [str(smoke), '--capture', f'--cycles={cycles}', f'--delay-ms={delay_ms}',
               f'--output-dir={directory}']
    started = datetime.now(timezone.utc).isoformat()
    failure = None
    with (directory / 'lifecycle.jsonl').open('x') as log:
        try:
            result = subprocess.run(command, env=env, stdout=log,
                                    timeout=cycles * (2 * delay_ms / 1000 + 60) + 30)
            status = result.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            status, failure = 1, str(error)
        except KeyboardInterrupt:
            status, failure = 130, 'Interrupted during collection'
    rows = []
    hashes = {}
    for cycle in range(1, cycles + 1):
        for attempt in (1, 2):
            path = directory / f'capture-{cycle:04d}-{attempt}.pgm'
            if path.is_file() and not path.is_symlink():
                rows.append({'path': path.name, 'finger_id': finger_id,
                             'session': session, 'condition': condition})
                hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    if len(rows) != cycles * 2 and status == 0:
        status, failure = 1, 'Capture process succeeded without writing every requested image'
    with (directory / 'manifest.csv').open('x', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=('path', 'finger_id', 'session', 'condition'))
        writer.writeheader()
        writer.writerows(rows)
    build_info = smoke.parent.parent / 'data/build-info.json'
    metadata = {'started_utc': started, 'finished_utc': datetime.now(timezone.utc).isoformat(),
                'driver': 'goodixtls55x4', 'finger_id': finger_id, 'session': session,
                'condition': condition, 'captures': len(rows), 'requested_captures': cycles * 2,
                'exit_status': status, 'error': failure, 'image_sha256': hashes,
                'build': json.loads(build_info.read_text()) if build_info.is_file() else None,
                'capture_executable_sha256': hashlib.sha256(smoke.read_bytes()).hexdigest()}
    (directory / 'session.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return status, len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='New private directory outside the source checkout')
    parser.add_argument('--smoke', type=Path,
                        default=Path('/opt/libfprint-goodix/libexec/libfprint-2/goodixtls-hardware-smoke'),
                        help='Capture binary (default: isolated Arch package helper)')
    parser.add_argument('--finger-id', required=True, help='Stable finger pseudonym, e.g. person1-right-index')
    parser.add_argument('--session', required=True, help='Distinct label for each recording session/day')
    parser.add_argument('--condition', required=True, help='normal, dry, partial, slow, etc.')
    parser.add_argument('--cycles', type=int, default=3, help='Two captures per open/close cycle (default: 3)')
    parser.add_argument('--delay-ms', type=int, default=15000)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 64 or not 1000 <= args.delay_ms <= 60000:
        parser.error('Use 1–64 cycles and a 1000–60000 ms deadline')
    for label in (args.finger_id, args.session, args.condition):
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,63}', label):
            parser.error('Labels must be 1–64 ASCII letters, digits, dots, underscores or hyphens')
    output = args.output.absolute()
    source = Path(__file__).resolve().parents[1]
    if output.resolve() == source or source in output.resolve().parents:
        parser.error('Keep biometric captures outside the source checkout')
    smoke = args.smoke.resolve()
    if not smoke.is_file() or not os.access(smoke, os.X_OK):
        parser.error('--smoke must identify a built executable')
    os.umask(0o077)
    try:
        status, count = collect(output, smoke, args.finger_id, args.session,
                                args.condition, args.cycles, args.delay_ms)
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')
    print(f'Saved {count} captures and manifest.csv in {output}')
    parser.exit(0 if status == 0 else 1)


if __name__ == '__main__':
    main()
